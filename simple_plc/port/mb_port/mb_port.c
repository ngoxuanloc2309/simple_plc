/*
 * mb_port.c - implements the porting layer (eMBP*) that libs/mb_master_slave
 * demands, on top of modbus_transport_t. See mb_port.h for the contract and
 * the per-tick order. Compiled only with SPLC_MODBUS_LIB == SPLC_MODBUS_LIB_MB.
 *
 * Everything runs in the engine context: no locking, no ISR.
 */
#include "mb_port.h"

#if SPLC_MODBUS_LIB == SPLC_MODBUS_LIB_MB

#include "mbport.h"
#include "mbs.h"
#include "common/mbportlayer.h"
#include "sx_time.h"
#include "splc_log.h"

static const char *TAG = "MB_PORT";

/* --- Tunables ----------------------------------------------------------- */

/* Baud rate handed to the library. It only sets the RTU T3.5 timer (> 19200 baud
 * => fixed 2 ms; 9600 => 4 ms, ...). The real baud belongs to the product's
 * UART and is NOT known here (decision 83); override with -D / splcopts.h when
 * the line is <= 19200 and the exact T3.5 matters. Floor: SPLC_MODBUS_RTU_GAP_MS. */
#ifndef SPLC_MODBUS_SERIAL_BAUD_HINT
#define SPLC_MODBUS_SERIAL_BAUD_HINT  115200UL
#endif

#define MB_PORT_SLOTS        (SPLC_MODBUS_MAX_TRANSPORTS)
#define MB_PORT_MAX_TIMERS   (3U * MB_PORT_SLOTS)   /* RTU/ASCII use <= 2 per server */
#define MB_PORT_MAX_EVENTS   (2U * MB_PORT_SLOTS)
#define MB_PORT_TX_MAX       520U   /* ASCII frame: ':' + 2*(addr+PDU+LRC) + CRLF */
#define MB_PORT_RX_CHUNK     32U
#define MB_PORT_RX_MAX_ITER  16U    /* bounds one tick to 512 bytes */
#define MB_PORT_WRITE_TIMEOUT_MS 5  /* idle timeout (no progress), same as nano's byte timeout */
#define MB_PORT_TCP_FEED_ITER    4U /* NewData calls per tick: header, body, +2 spare */

/* --- State --------------------------------------------------------------- */

typedef struct {
    const modbus_transport_t     *t;         /* not owned; NULL = slot free */
    xMBSHandle                    server;    /* eMBSSerialInit() result */
    xMBHandle                     lib_hdl;   /* handle the library passes to callbacks */
    pvMBPSerialReceiverCB         rx_cb;     /* NULL = receiver disabled */
    pbMBPSerialTransmitterEmptyCB tx_cb;
    bool                          tx_pending;
    bool                          open;      /* eMBPSerialInit() done */
    bool                          created;   /* server fully created */
    /* TCP (kind == TCP): one always-connected virtual client over the byte pipe */
    bool                          tcp;           /* eMBPTCPServerInit() done */
    bool                          tcp_up;        /* virtual client registered in the library */
    peMBPTCPClientNewDataCB       tcp_newdata;
    peMBPTCPClientDisconnectedCB  tcp_disc;
    peMBPTCPClientConnectedCB     tcp_conn;
    uint16_t                      tcp_got;       /* bytes eMBPTCPConRead() returned in this call */
    uint16_t                      tcp_frame;     /* bytes taken since the last reply / reset */
    uint32_t                      tcp_rx_ms;     /* tick of the last byte taken */
} mb_slot_t;

typedef struct {
    bool                 used;
    bool                 armed;
    uint16_t             timeout_ms;
    uint32_t             start_ms;
    pbMBPTimerExpiredCB  cb;
    xMBHandle            hdl;
} mb_timer_t;

typedef struct {
    bool          used;
    bool          pending;
    xMBPEventType ev;
} mb_event_t;

static mb_slot_t            s_slot[MB_PORT_SLOTS];
static mb_timer_t           s_timer[MB_PORT_MAX_TIMERS];
static mb_event_t           s_event[MB_PORT_MAX_EVENTS];
static uint8_t              s_tx_buf[MB_PORT_TX_MAX];   /* one flush at a time */
static mb_port_holding_cb_t s_holding_cb;
static uint8_t              s_tcp_pending = 0xFFU;      /* slot inside eMBSTCPInit(), else 0xFF */

/* --- Library callback: holding registers -> product register map -------- */

static eMBException holding_tramp(UBYTE *buf, USHORT addr, USHORT nregs, eMBSRegisterMode mode)
{
    if (s_holding_cb == NULL) {
        return MB_PDU_EX_SLAVE_DEVICE_FAILURE;
    }
    const uint8_t ex = s_holding_cb(buf, addr, nregs, mode == MBS_REGISTER_WRITE);
    switch (ex) {
    case 0: return MB_PDU_EX_NONE;
    case 1: return MB_PDU_EX_ILLEGAL_FUNCTION;
    case 2: return MB_PDU_EX_ILLEGAL_DATA_ADDRESS;
    case 3: return MB_PDU_EX_ILLEGAL_DATA_VALUE;
    default: return MB_PDU_EX_SLAVE_DEVICE_FAILURE;
    }
}

/* --- eMBPTimer*: soft timers on sx_get_tick_ms() ------------------------- */

static uint16_t floor_gap(uint16_t ms)
{
    /* The floor applies to every timer: the T3.5 one is the only short one the
     * server creates (the wait-after-send timer is compiled out, ASCII ones
     * are >= 1 s, which the floor never touches). */
    return (ms < (uint16_t)SPLC_MODBUS_RTU_GAP_MS) ? (uint16_t)SPLC_MODBUS_RTU_GAP_MS : ms;
}

eMBErrorCode eMBPTimerInit(xMBPTimerHandle *xTimerHdl, USHORT usTimeOut1ms,
                           pbMBPTimerExpiredCB pbMBPTimerExpiredFN, xMBHandle xHdl)
{
    if (xTimerHdl == NULL || pbMBPTimerExpiredFN == NULL || xHdl == MBP_HDL_INVALID) {
        return MB_EINVAL;
    }
    for (uint32_t i = 0; i < MB_PORT_MAX_TIMERS; i++) {
        if (!s_timer[i].used) {
            s_timer[i].used       = true;
            s_timer[i].armed      = false;      /* timers start disarmed */
            s_timer[i].timeout_ms = floor_gap(usTimeOut1ms);
            s_timer[i].cb         = pbMBPTimerExpiredFN;
            s_timer[i].hdl        = xHdl;
            *xTimerHdl = &s_timer[i];
            return MB_ENOERR;
        }
    }
    return MB_ENORES;
}

void vMBPTimerClose(xMBPTimerHandle xTimerHdl)
{
    mb_timer_t *tm = (mb_timer_t *)xTimerHdl;
    if (tm != NULL) {
        tm->used  = false;
        tm->armed = false;
    }
}

eMBErrorCode eMBPTimerSetTimeout(xMBPTimerHandle xTimerHdl, USHORT usTimeOut1ms)
{
    mb_timer_t *tm = (mb_timer_t *)xTimerHdl;
    if (tm == NULL || !tm->used) {
        return MB_EINVAL;
    }
    tm->timeout_ms = floor_gap(usTimeOut1ms);
    return MB_ENOERR;
}

eMBErrorCode eMBPTimerStart(xMBPTimerHandle xTimerHdl)
{
    mb_timer_t *tm = (mb_timer_t *)xTimerHdl;
    if (tm == NULL || !tm->used) {
        return MB_EINVAL;
    }
    tm->start_ms = sx_get_tick_ms();   /* starting a running timer restarts it */
    tm->armed    = true;
    return MB_ENOERR;
}

eMBErrorCode eMBPTimerStop(xMBPTimerHandle xTimerHdl)
{
    mb_timer_t *tm = (mb_timer_t *)xTimerHdl;
    if (tm == NULL || !tm->used) {
        return MB_EINVAL;
    }
    tm->armed = false;
    return MB_ENOERR;
}

/* Runs the expired timers that belong to one server (matched by handle, so a
 * slot never fires another slot's timer before that slot has fed its RX bytes). */
static void timers_run(xMBHandle hdl)
{
    const uint32_t now = sx_get_tick_ms();
    for (uint32_t i = 0; i < MB_PORT_MAX_TIMERS; i++) {
        mb_timer_t *tm = &s_timer[i];
        if (tm->used && tm->armed && tm->hdl == hdl &&
            (uint32_t)(now - tm->start_ms) >= (uint32_t)tm->timeout_ms) {
            tm->armed = false;           /* single shot, before the callback */
            (void)tm->cb(tm->hdl);
        }
    }
}

/* --- eMBPEvent*: one-slot event "queue" (a new event overwrites) --------- */

eMBErrorCode eMBPEventCreate(xMBPEventHandle *pxEventHdl)
{
    if (pxEventHdl == NULL) {
        return MB_EINVAL;
    }
    for (uint32_t i = 0; i < MB_PORT_MAX_EVENTS; i++) {
        if (!s_event[i].used) {
            s_event[i].used    = true;
            s_event[i].pending = false;
            *pxEventHdl = &s_event[i];
            return MB_ENOERR;
        }
    }
    return MB_ENORES;
}

eMBErrorCode eMBPEventPost(const xMBPEventHandle xEventHdl, xMBPEventType xEvent)
{
    mb_event_t *ev = (mb_event_t *)xEventHdl;
    if (ev == NULL || !ev->used) {
        return MB_EINVAL;
    }
    ev->ev      = xEvent;
    ev->pending = true;
    return MB_ENOERR;
}

BOOL bMBPEventGet(const xMBPEventHandle xEventHdl, xMBPEventType *pxEvent)
{
    mb_event_t *ev = (mb_event_t *)xEventHdl;
    if (ev == NULL || !ev->used || !ev->pending || pxEvent == NULL) {
        return FALSE;
    }
    *pxEvent    = ev->ev;
    ev->pending = false;
    return TRUE;
}

void vMBPEventDelete(xMBPEventHandle xEventHdl)
{
    mb_event_t *ev = (mb_event_t *)xEventHdl;
    if (ev != NULL) {
        ev->used    = false;
        ev->pending = false;
    }
}

/* --- eMBPSerial*: a slot of modbus_transport_t --------------------------- */

eMBErrorCode eMBPSerialInit(xMBPSerialHandle *pxSerialHdl, UCHAR ucPort, ULONG ulBaudRate,
                            UCHAR ucDataBits, eMBSerialParity eParity, UCHAR ucStopBits,
                            xMBHandle xMBHdl)
{
    (void)ulBaudRate; (void)ucDataBits; (void)eParity; (void)ucStopBits; /* link owns the UART */
    if (pxSerialHdl == NULL || ucPort >= MB_PORT_SLOTS || xMBHdl == MBP_HDL_INVALID) {
        return MB_EINVAL;
    }
    mb_slot_t *sl = &s_slot[ucPort];
    if (sl->t == NULL || sl->open) {      /* slot must be bound by mb_port_server_create() */
        return MB_EINVAL;
    }
    sl->lib_hdl    = xMBHdl;
    sl->rx_cb      = NULL;
    sl->tx_cb      = NULL;
    sl->tx_pending = false;
    sl->open       = true;
    *pxSerialHdl   = sl;
    return MB_ENOERR;
}

eMBErrorCode eMBPSerialClose(xMBPSerialHandle xSerialHdl)
{
    mb_slot_t *sl = (mb_slot_t *)xSerialHdl;
    if (sl == NULL || !sl->open) {
        return MB_EINVAL;
    }
    sl->open       = false;
    sl->rx_cb      = NULL;
    sl->tx_cb      = NULL;
    sl->tx_pending = false;
    return MB_ENOERR;
}

eMBErrorCode eMBPSerialTxEnable(xMBPSerialHandle xSerialHdl, pbMBPSerialTransmitterEmptyCB pbFN)
{
    mb_slot_t *sl = (mb_slot_t *)xSerialHdl;
    if (sl == NULL || !sl->open) {
        return MB_EINVAL;
    }
    sl->tx_cb      = pbFN;
    sl->tx_pending = (pbFN != NULL);   /* drained by tx_flush() AFTER eMBSPoll() */
    return MB_ENOERR;
}

eMBErrorCode eMBPSerialRxEnable(xMBPSerialHandle xSerialHdl, pvMBPSerialReceiverCB pvFN)
{
    mb_slot_t *sl = (mb_slot_t *)xSerialHdl;
    if (sl == NULL || !sl->open) {
        return MB_EINVAL;
    }
    sl->rx_cb = pvFN;
    return MB_ENOERR;
}


/* --- eMBPTCP*: ONE virtual client on a slot's byte pipe ------------------ */
/*
 * The host owns sockets (it accepts, replaces, drops connections); a
 * modbus_transport_t only gives read()/write(). So the library sees a server
 * with exactly one client that is always connected:
 *   - eMBPTCPServerInit() only remembers the callbacks (the library has not
 *     finished its own init yet, so the client is registered on the first tick);
 *   - NewData is called every tick without a "data available" probe: the
 *     library asks eMBPTCPConRead(), which is transport->read(timeout 0) and
 *     simply returns 0 bytes when the pipe is empty;
 *   - eMBPTCPConWrite() is transport->write().
 * Server handle = the slot, client handle = the address of slot->tcp_up.
 */

eMBErrorCode eMBPTCPServerInit(xMBPTCPHandle *pxTCPHdl, CHAR *pcBindAddress, USHORT usTCPPort,
                               xMBHandle xMBHdl, peMBPTCPClientNewDataCB eMBPTCPClientNewDataFN,
                               peMBPTCPClientDisconnectedCB eMBPTCPClientDisconnectedFN,
                               peMBPTCPClientConnectedCB eMBPTCPClientConnectedFN)
{
    (void)pcBindAddress; (void)usTCPPort;          /* the host owns the socket */
    if (pxTCPHdl == NULL || xMBHdl == MBP_HDL_INVALID || eMBPTCPClientNewDataFN == NULL ||
        eMBPTCPClientDisconnectedFN == NULL || eMBPTCPClientConnectedFN == NULL ||
        s_tcp_pending >= MB_PORT_SLOTS) {
        return MB_EINVAL;
    }
    mb_slot_t *sl = &s_slot[s_tcp_pending];
    if (sl->t == NULL || sl->tcp) {
        return MB_EINVAL;
    }
    sl->lib_hdl     = xMBHdl;
    sl->tcp_newdata = eMBPTCPClientNewDataFN;
    sl->tcp_disc    = eMBPTCPClientDisconnectedFN;
    sl->tcp_conn    = eMBPTCPClientConnectedFN;
    sl->tcp_up      = false;
    sl->tcp_got     = 0U;
    sl->tcp_frame   = 0U;
    sl->tcp         = true;
    *pxTCPHdl       = sl;
    return MB_ENOERR;
}

eMBErrorCode eMBTCPServerClose(xMBPTCPHandle xTCPHdl)
{
    mb_slot_t *sl = (mb_slot_t *)xTCPHdl;
    if (sl == NULL || !sl->tcp) {
        return MB_EINVAL;
    }
    sl->tcp         = false;
    sl->tcp_up      = false;
    sl->tcp_newdata = NULL;
    sl->tcp_disc    = NULL;
    sl->tcp_conn    = NULL;
    return MB_ENOERR;
}

static bool tcp_handles_ok(const mb_slot_t *sl, xMBPTCPHandle h, xMBPTCPClientHandle c)
{
    return sl != NULL && sl->tcp && h == (xMBPTCPHandle)sl && c == (xMBPTCPClientHandle)&sl->tcp_up;
}

eMBErrorCode eMBPTCPConRead(xMBPTCPHandle xTCPHdl, xMBPTCPClientHandle xTCPClientHdl,
                            UBYTE *pubBuffer, USHORT *pusBufferLen, USHORT usBufferMax)
{
    mb_slot_t *sl = (mb_slot_t *)xTCPHdl;
    if (!tcp_handles_ok(sl, xTCPHdl, xTCPClientHdl) || pubBuffer == NULL || pusBufferLen == NULL) {
        return MB_EINVAL;
    }
    *pusBufferLen = 0U;
    if (usBufferMax == 0U) {
        return MB_ENOERR;
    }
    const int32_t n = sl->t->read(pubBuffer, usBufferMax, 0, sl->t->ctx);
    if (n < 0) {
        return MB_EIO;                 /* the library drops the client; we re-open it next tick */
    }
    if (n > 0) {
        const uint16_t got = (n > (int32_t)usBufferMax) ? usBufferMax : (uint16_t)n;
        *pusBufferLen = got;
        sl->tcp_got   = (uint16_t)(sl->tcp_got + got);
        sl->tcp_frame = (uint16_t)(sl->tcp_frame + got);
        sl->tcp_rx_ms = sx_get_tick_ms();
    }
    return MB_ENOERR;
}

eMBErrorCode eMBPTCPConWrite(xMBPTCPHandle xTCPHdl, xMBPTCPClientHandle xTCPClientHdl,
                             const UBYTE *pubBuffer, USHORT usBufferLen)
{
    mb_slot_t *sl = (mb_slot_t *)xTCPHdl;
    if (!tcp_handles_ok(sl, xTCPHdl, xTCPClientHdl) || pubBuffer == NULL) {
        return MB_EINVAL;
    }
    sl->tcp_frame = 0U;                /* the request is answered: nothing is half-received */
    const int32_t w = sl->t->write(pubBuffer, usBufferLen, MB_PORT_WRITE_TIMEOUT_MS, sl->t->ctx);
    if (w != (int32_t)usBufferLen) {
        log_warn(TAG, "tcp reply write: %ld of %u bytes", (long)w, (unsigned)usBufferLen);
        return MB_EIO;
    }
    return MB_ENOERR;
}

eMBErrorCode eMBPTCPConClose(xMBPTCPHandle xTCPHdl, xMBPTCPClientHandle xTCPClientHdl)
{
    mb_slot_t *sl = (mb_slot_t *)xTCPHdl;
    if (!tcp_handles_ok(sl, xTCPHdl, xTCPClientHdl)) {
        return MB_EINVAL;
    }
    /* The pipe stays open; the library forgot the client, so re-register it on
     * the next tick (tcp_poll). Does NOT call back into the library. */
    sl->tcp_up    = false;
    sl->tcp_frame = 0U;
    return MB_ENOERR;
}

/* --- Library load/unload ------------------------------------------------- */

void vMBPLibraryLoad(void)   { }
void vMBPLibraryUnload(void) { }

/* --- Per-tick pump ------------------------------------------------------- */

static void rx_feed(mb_slot_t *sl)
{
    uint8_t chunk[MB_PORT_RX_CHUNK];
    for (uint32_t it = 0; it < MB_PORT_RX_MAX_ITER; it++) {
        if (sl->rx_cb == NULL) {
            return;                      /* leave the bytes in the link until RX is on again */
        }
        const int32_t n = sl->t->read(chunk, (uint16_t)sizeof chunk, 0, sl->t->ctx);
        if (n <= 0) {
            return;
        }
        for (int32_t i = 0; i < n && sl->rx_cb != NULL; i++) {
            sl->rx_cb(sl->lib_hdl, chunk[i]);
        }
        if ((uint32_t)n < sizeof chunk) {
            return;
        }
    }
}

static void tx_flush(mb_slot_t *sl)
{
    if (!sl->tx_pending || sl->tx_cb == NULL) {
        return;
    }
    sl->tx_pending = false;
    const pbMBPSerialTransmitterEmptyCB cb = sl->tx_cb;
    uint32_t n = 0;
    UBYTE    b = 0;
    /* The library hands out one byte per call and returns FALSE after the last
     * one (it re-enables its receiver on that call). A length-0 "dummy
     * transmission" returns FALSE at once: nothing is written. */
    while (n < MB_PORT_TX_MAX && cb(sl->lib_hdl, &b)) {
        s_tx_buf[n++] = b;
    }
    if (n >= MB_PORT_TX_MAX) {
        log_error(TAG, "reply longer than %u bytes dropped", (unsigned)MB_PORT_TX_MAX);
        return;
    }
    if (n > 0U) {
        const int32_t w = sl->t->write(s_tx_buf, (uint16_t)n, MB_PORT_WRITE_TIMEOUT_MS, sl->t->ctx);
        if (w != (int32_t)n) {
            log_warn(TAG, "reply write: %ld of %u bytes", (long)w, (unsigned)n);
        }
    }
}


/* One tick of a TCP slot: (re)register the virtual client, drop a stale partial
 * frame, let the library take what the pipe has, then run it. The reply goes
 * out from inside eMBSPoll() through eMBPTCPConWrite(). */
static void tcp_poll(mb_slot_t *sl)
{
    if (sl->tcp_frame > 0U &&
        (uint32_t)(sx_get_tick_ms() - sl->tcp_rx_ms) >= (uint32_t)SPLC_MODBUS_TCP_PARTIAL_MS) {
        log_debug(TAG, "tcp: %u byte(s) of an unfinished request dropped", (unsigned)sl->tcp_frame);
        (void)sl->tcp_disc(sl->lib_hdl, (xMBPTCPClientHandle)&sl->tcp_up);   /* may reach ConClose */
        sl->tcp_up    = false;
        sl->tcp_frame = 0U;
    }
    if (!sl->tcp_up) {
        sl->tcp_up = (sl->tcp_conn(sl->lib_hdl, (xMBPTCPClientHandle)&sl->tcp_up) == MB_ENOERR);
        if (!sl->tcp_up) {
            return;
        }
    }
    for (uint32_t it = 0; it < MB_PORT_TCP_FEED_ITER && sl->tcp_up; it++) {
        sl->tcp_got = 0U;
        (void)sl->tcp_newdata(sl->lib_hdl, (xMBPTCPClientHandle)&sl->tcp_up);
        if (sl->tcp_got == 0U) {
            break;                       /* nothing read: pipe empty, or the library is busy */
        }
    }
    (void)eMBSPoll(sl->server);
}

bool mb_port_server_create(uint8_t slot, const modbus_transport_t *t,
                           mb_port_holding_cb_t holding_cb)
{
    if (slot >= MB_PORT_SLOTS || t == NULL || t->read == NULL || t->write == NULL ||
        holding_cb == NULL || s_slot[slot].created) {
        log_error(TAG, "create: bad argument or slot %u already used", (unsigned)slot);
        return false;
    }
    if (t->kind == MODBUS_TRANSPORT_KIND_TCP) {
        mb_slot_t *tl = &s_slot[slot];
        s_holding_cb  = holding_cb;
        tl->t         = t;
        s_tcp_pending = slot;
        eMBErrorCode te = eMBSTCPInit(&tl->server, NULL, 0U);
        s_tcp_pending = 0xFFU;
        if (te != MB_ENOERR) {
            log_error(TAG, "eMBSTCPInit failed: err=%d", (int)te);
            tl->t = NULL;
            return false;
        }
        te = eMBSRegisterHoldingCB(tl->server, holding_tramp);
        if (te != MB_ENOERR) {
            log_error(TAG, "eMBSRegisterHoldingCB failed: err=%d", (int)te);
            (void)eMBSClose(tl->server);
            tl->t = NULL;
            return false;
        }
        tl->created = true;
        log_info(TAG, "slot %u: TCP server up (one client, unit id ignored)", (unsigned)slot);
        return true;
    }
    eMBSerialMode mode;
    if (t->kind == MODBUS_TRANSPORT_KIND_RTU) {
        mode = MB_RTU;
    } else if (t->kind == MODBUS_TRANSPORT_KIND_ASCII) {
        mode = MB_ASCII;
    } else {
        log_error(TAG, "create: transport kind %d not supported by the MB adapter yet "
                       "(RTU, ASCII and TCP; UDP is not written)", (int)t->kind);
        return false;
    }

    mb_slot_t *sl = &s_slot[slot];
    s_holding_cb  = holding_cb;
    sl->t         = t;

    eMBErrorCode e = eMBSSerialInit(&sl->server, mode, t->unit_id, slot,
                                    SPLC_MODBUS_SERIAL_BAUD_HINT, MB_PAR_NONE);
    if (e != MB_ENOERR) {
        log_error(TAG, "eMBSSerialInit failed: err=%d unit_id=%u", (int)e, (unsigned)t->unit_id);
        sl->t = NULL;
        return false;
    }
    e = eMBSRegisterHoldingCB(sl->server, holding_tramp);
    if (e != MB_ENOERR) {
        log_error(TAG, "eMBSRegisterHoldingCB failed: err=%d", (int)e);
        (void)eMBSClose(sl->server);
        sl->t = NULL;
        return false;
    }
    sl->created = true;
    log_info(TAG, "slot %u: %s server up, unit_id=%u", (unsigned)slot,
             (mode == MB_RTU) ? "RTU" : "ASCII", (unsigned)t->unit_id);
    return true;
}

void mb_port_server_poll(uint8_t slot)
{
    if (slot >= MB_PORT_SLOTS || !s_slot[slot].created) {
        return;
    }
    mb_slot_t *sl = &s_slot[slot];
    if (sl->tcp) {
        tcp_poll(sl);
        return;
    }
    rx_feed(sl);                    /* 2. bytes first ...                       */
    timers_run(sl->lib_hdl);        /* 3. ... then timers (order matters: 86)   */
    (void)eMBSPoll(sl->server);     /* 4. MBS_POLL_SINGLE_CYCLE=1: runs to idle */
    tx_flush(sl);                   /* 5. reply out, library re-enables RX      */
}

#endif /* SPLC_MODBUS_LIB == SPLC_MODBUS_LIB_MB */