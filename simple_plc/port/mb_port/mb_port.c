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
/* RS-485 channels (Modbus master over a modbus_serial_port_t): one per Modbus master
 * port plus one for the Gateway's Remote I/O master. Channel index = MB_PORT_SLOTS + n,
 * so the library's `ucPort` selects either kind of byte pipe. */
/* Slave serving (plc_mbs): one more channel per port that can take the Slave role. */
#if SPLC_MBM_ENABLE
#define MB_PORT_RS485_MASTER_CH (SPLC_MBM_PORT_COUNT + 1U)
#else
#define MB_PORT_RS485_MASTER_CH (1U)
#endif
#if SPLC_MODBUS_SLAVE_ENABLE
#define MB_PORT_RS485_SLAVE_CH  (SPLC_MBM_PORT_COUNT)
#else
#define MB_PORT_RS485_SLAVE_CH  (0U)
#endif
/* Zigbee byte pipe (plc_zigbee): one channel for its Slave endpoint (plc_mbs) ... */
#if SPLC_ZIGBEE_ENABLE && SPLC_MODBUS_SLAVE_ENABLE
#define MB_PORT_ZB_SLAVE_CH  (1U)
#else
#define MB_PORT_ZB_SLAVE_CH  (0U)
#endif
/* ... and one as a Modbus MASTER (MB_READ / MB_WRITE port value 4). */
#if SPLC_ZIGBEE_ENABLE && SPLC_MBM_ENABLE
#define MB_PORT_ZB_MASTER_CH (1U)
#else
#define MB_PORT_ZB_MASTER_CH (0U)
#endif
/* Length-framed RESPONSES on a master channel exist only with a Zigbee port (a build without it
 * keeps its exact size). */
#define MB_PORT_RESP_FRAMING (SPLC_ZIGBEE_ENABLE && SPLC_MBM_ENABLE)
#define MB_PORT_RS485_CH     (MB_PORT_RS485_MASTER_CH + MB_PORT_RS485_SLAVE_CH + MB_PORT_ZB_SLAVE_CH + MB_PORT_ZB_MASTER_CH)
/* TCP master channels (host-owned sockets) follow the RS-485 ones. */
#if SPLC_MODBUS_TCP_MASTER_ENABLE
#define MB_PORT_TCPM_CH      (SPLC_MODBUS_TCP_MASTER_COUNT)
#else
#define MB_PORT_TCPM_CH      (0U)
#endif
#define MB_PORT_SER_END      (MB_PORT_SLOTS + MB_PORT_RS485_CH)   /* end of the serial range */
#define MB_PORT_CHANNELS     (MB_PORT_SER_END + MB_PORT_TCPM_CH)
#define MB_PORT_MAX_TIMERS   (3U * MB_PORT_CHANNELS)   /* RTU/ASCII use <= 3 per instance */
#define MB_PORT_MAX_EVENTS   (2U * MB_PORT_CHANNELS)
#if SPLC_MBM_ASCII_PORTS > 0
#define MB_PORT_SER_TX_MAX   520U   /* ASCII frame: ':' + 2*(addr+PDU+LRC) + CRLF = 513 at most */
#else
#define MB_PORT_SER_TX_MAX   256U   /* RTU ADU maximum */
#endif
#define MB_PORT_ECHO_DRAIN   16U    /* bounded RX drain after a blocking transmit (echo) */
#define MB_PORT_TX_MAX       520U   /* ASCII frame: ':' + 2*(addr+PDU+LRC) + CRLF */
#define MB_PORT_RX_CHUNK     32U
#define MB_PORT_RX_MAX_ITER  16U    /* bounds one tick to 512 bytes */
#define MB_PORT_WRITE_TIMEOUT_MS 5  /* idle timeout (no progress), same as nano's byte timeout */
#define MB_PORT_TCP_FEED_ITER    4U /* NewData calls per tick: header, body, +2 spare */

/* --- State --------------------------------------------------------------- */

typedef struct {
    const modbus_transport_t     *t;         /* not owned; NULL = slot free */
    /* RS-485 channel (t == NULL): the port of the Modbus master, not owned */
    const modbus_serial_port_t   *sp;
    uint32_t                      sp_baud;   /* for the transmit-time estimate */
    uint8_t                       sp_tx[MB_PORT_SER_TX_MAX]; /* frame kept alive for an asynchronous send */
    bool                          sp_async;     /* write_start() issued, tx_done() not seen yet */
    uint32_t                      sp_async_dl;  /* give-up tick for that transfer */
    xMBSHandle                    server;    /* eMBSSerialInit() result */
    xMBHandle                     lib_hdl;   /* handle the library passes to callbacks */
    pvMBPSerialReceiverCB         rx_cb;     /* NULL = receiver disabled */
    uint8_t                       rx_pend[MB_PORT_RX_CHUNK]; /* read from the pipe, not yet given to the library */
    uint8_t                       rx_pend_n, rx_pend_i;
    /* RTU frame end by length (SPLC_MODBUS_SYNC_LENGTH) */
    bool                          fr_len;     /* this slot finds the end of a request from its length */
#if MB_PORT_RESP_FRAMING
    bool                          fr_resp;    /* fr_len on a MASTER channel: the frame in progress is a RESPONSE */
#endif
    bool                          fr_hold;    /* a whole request is in: feed nothing until it is answered */
    uint16_t                      fr_total, fr_need;
    uint32_t                      fr_last_ms; /* time of the last byte of the frame in progress */
    uint8_t                       fr_hdr[7];
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
#if SPLC_MODBUS_TCP_MASTER_ENABLE
    /* TCP master channel: the host's socket behind three callbacks (tcp_up/tcp_newdata/
     * tcp_disc/tcp_got are shared with the TCP-server fields above). */
    const mb_port_tcpc_ops_t     *tc;            /* not owned; NULL = channel free */
    bool                          tcpc;          /* eMBPTCPClientInit() done */
    bool                          tcpc_lost;     /* connection failed since take_lost() */
#endif
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

static mb_slot_t            s_slot[MB_PORT_CHANNELS];
static mb_timer_t           s_timer[MB_PORT_MAX_TIMERS];
static mb_event_t           s_event[MB_PORT_MAX_EVENTS];
static uint8_t              s_tx_buf[MB_PORT_TX_MAX];   /* one flush at a time */
static mb_port_holding_cb_t s_holding_cb;
#if SPLC_MODBUS_SLAVE_ENABLE
static mb_port_slave_ops_t  s_slave_ops;                /* plc_mbs callbacks (one map, all ports) */
#endif
static uint8_t              s_tcp_pending = 0xFFU;      /* slot inside eMBSTCPInit(), else 0xFF */
#if SPLC_MODBUS_TCP_MASTER_ENABLE
static int                  s_tcpc_pending = -1;        /* channel inside eMBMTCPInit(), else -1 */
#endif

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

#if SPLC_MODBUS_SLAVE_ENABLE
/* App link: coils / discrete inputs are not served. A callback that says "illegal function"
 * keeps the answer 0x01 (with no callback the library answers 0x02). */
static eMBException applink_coil_tramp(UBYTE *buf, USHORT addr, USHORT n, eMBSRegisterMode mode)
{ (void)buf; (void)addr; (void)n; (void)mode; return MB_PDU_EX_ILLEGAL_FUNCTION; }
static eMBException applink_disc_tramp(UBYTE *buf, USHORT addr, USHORT n)
{ (void)buf; (void)addr; (void)n; return MB_PDU_EX_ILLEGAL_FUNCTION; }

/* Slave serving (plc_mbs): exceptions 1..3 and 6 (busy: a diagnostic session holds the tags). */
static eMBException slave_ex(uint8_t ex)
{
    switch (ex) {
    case 0: return MB_PDU_EX_NONE;
    case 1: return MB_PDU_EX_ILLEGAL_FUNCTION;
    case 2: return MB_PDU_EX_ILLEGAL_DATA_ADDRESS;
    case 3: return MB_PDU_EX_ILLEGAL_DATA_VALUE;
    case 6: return MB_PDU_EX_SLAVE_BUSY;
    default: return MB_PDU_EX_SLAVE_DEVICE_FAILURE;
    }
}
static eMBException slave_holding_tramp(UBYTE *buf, USHORT addr, USHORT n, eMBSRegisterMode mode)
{ return (s_slave_ops.holding == NULL) ? MB_PDU_EX_SLAVE_DEVICE_FAILURE
         : slave_ex(s_slave_ops.holding(buf, addr, n, mode == MBS_REGISTER_WRITE)); }
static eMBException slave_coil_tramp(UBYTE *buf, USHORT addr, USHORT n, eMBSRegisterMode mode)
{ return (s_slave_ops.coils == NULL) ? MB_PDU_EX_SLAVE_DEVICE_FAILURE
         : slave_ex(s_slave_ops.coils(buf, addr, n, mode == MBS_REGISTER_WRITE)); }
static eMBException slave_disc_tramp(UBYTE *buf, USHORT addr, USHORT n)
{ return (s_slave_ops.discrete == NULL) ? MB_PDU_EX_SLAVE_DEVICE_FAILURE
         : slave_ex(s_slave_ops.discrete(buf, addr, n, false)); }
#endif /* SPLC_MODBUS_SLAVE_ENABLE */

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
    if (pxSerialHdl == NULL || ucPort >= MB_PORT_CHANNELS || xMBHdl == MBP_HDL_INVALID) {
        return MB_EINVAL;
    }
    mb_slot_t *sl = &s_slot[ucPort];
    /* bound by mb_port_server_create() (transport) or mb_port_serial_attach() (RS-485) */
    if ((sl->t == NULL && sl->sp == NULL) || sl->open) {
        return MB_EINVAL;
    }
    sl->lib_hdl    = xMBHdl;
    sl->rx_cb      = NULL;
    sl->rx_pend_n  = sl->rx_pend_i = 0U;
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
    sl->rx_pend_n  = sl->rx_pend_i = 0U;
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
    if (pvFN != NULL) { sl->fr_total = 0U; sl->fr_need = 0U; sl->fr_hold = false; }   /* a new frame starts */
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
#if SPLC_MODBUS_TCP_MASTER_ENABLE
    if (sl != NULL && sl->tcpc && h == (xMBPTCPHandle)sl && c == (xMBPTCPClientHandle)&sl->tcp_up) {
        return true;
    }
#endif
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
#if SPLC_MODBUS_TCP_MASTER_ENABLE
    if (sl->tcpc) {                    /* TCP master: the host's socket */
        const int rn = sl->tc->read(sl->tc->ctx, pubBuffer, usBufferMax);
        if (rn < 0) {
            sl->tcpc_lost = true;
            return MB_EIO;             /* the library closes the client; the facade sees `lost` */
        }
        if (rn > 0) {
            const uint16_t rgot = ((uint32_t)rn > usBufferMax) ? usBufferMax : (uint16_t)rn;
            *pusBufferLen = rgot;
            sl->tcp_got   = (uint16_t)(sl->tcp_got + rgot);
        }
        return MB_ENOERR;
    }
#endif
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
#if SPLC_MODBUS_TCP_MASTER_ENABLE
    if (sl->tcpc) {                    /* TCP master: one request frame out */
        const int rw = sl->tc->write(sl->tc->ctx, pubBuffer, usBufferLen);
        if (rw != (int)usBufferLen) {
            log_warn(TAG, "tcp master write: %d of %u bytes", rw, (unsigned)usBufferLen);
            sl->tcpc_lost = true;
            return MB_EIO;
        }
        return MB_ENOERR;
    }
#endif
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
#if SPLC_MODBUS_TCP_MASTER_ENABLE
    if (sl->tcpc) {                    /* TCP master: really close the host's connection */
        if (sl->tcp_up) { sl->tc->close(sl->tc->ctx); }
        sl->tcp_up = false;
        return MB_ENOERR;
    }
#endif
    /* The pipe stays open; the library forgot the client, so re-register it on
     * the next tick (tcp_poll). Does NOT call back into the library. */
    sl->tcp_up    = false;
    sl->tcp_frame = 0U;
    return MB_ENOERR;
}

#if SPLC_MODBUS_TCP_MASTER_ENABLE
/* --- eMBPTCPClient*: TCP master over a host-owned socket -------------------- */
/* The facade (modbus_tcp_master.c) connects through the host BEFORE it calls the library's
 * eMBMTCPConnect(); eMBPTCPClientOpen() therefore opens nothing, it only makes the already
 * connected socket the library's client. Server handle = the slot, client handle = &tcp_up. */
eMBErrorCode eMBPTCPClientInit(xMBPTCPHandle *pxTCPHdl, xMBHandle xMBHdl,
                               peMBPTCPClientNewDataCB eMBPTCPClientNewDATAFN,
                               peMBPTCPClientDisconnectedCB eMBPTCPClientDisconnectedFN)
{
    if (pxTCPHdl == NULL || xMBHdl == MBP_HDL_INVALID || eMBPTCPClientNewDATAFN == NULL ||
        eMBPTCPClientDisconnectedFN == NULL || s_tcpc_pending < (int)MB_PORT_SER_END ||
        s_tcpc_pending >= (int)MB_PORT_CHANNELS) {
        return MB_EINVAL;
    }
    mb_slot_t *sl = &s_slot[s_tcpc_pending];
    if (sl->tc == NULL || sl->tcpc) {
        return MB_EINVAL;
    }
    sl->lib_hdl     = xMBHdl;
    sl->tcp_newdata = eMBPTCPClientNewDATAFN;
    sl->tcp_disc    = eMBPTCPClientDisconnectedFN;
    sl->tcp_up      = false;
    sl->tcp_got     = 0U;
    sl->tcpc_lost   = false;
    sl->tcpc        = true;
    *pxTCPHdl       = sl;
    return MB_ENOERR;
}

eMBErrorCode eMBPTCPClientClose(xMBPTCPHandle xTCPHdl)
{
    mb_slot_t *sl = (mb_slot_t *)xTCPHdl;
    if (sl == NULL || !sl->tcpc) {
        return MB_EINVAL;
    }
    if (sl->tcp_up) { sl->tc->close(sl->tc->ctx); }
    sl->tcpc        = false;
    sl->tcp_up      = false;
    sl->tcp_newdata = NULL;
    sl->tcp_disc    = NULL;
    return MB_ENOERR;
}

eMBErrorCode eMBPTCPClientOpen(xMBPTCPHandle xTCPHdl, xMBPTCPClientHandle *pxTCPClientHdl,
                               const CHAR *pcConnectAddress, USHORT usTCPPort)
{
    (void)pcConnectAddress; (void)usTCPPort;       /* the host already connected */
    mb_slot_t *sl = (mb_slot_t *)xTCPHdl;
    if (sl == NULL || !sl->tcpc || pxTCPClientHdl == NULL) {
        return MB_EINVAL;
    }
    sl->tcp_up     = true;
    sl->tcpc_lost  = false;
    *pxTCPClientHdl = (xMBPTCPClientHandle)&sl->tcp_up;
    return MB_ENOERR;
}
#endif /* SPLC_MODBUS_TCP_MASTER_ENABLE */

/* --- Library load/unload ------------------------------------------------- */

void vMBPLibraryLoad(void)   { }
void vMBPLibraryUnload(void) { }

/* --- Per-tick pump ------------------------------------------------------- */

/* Byte pipe of a channel: a transport slot or an RS-485 port. Never blocks. */
static int32_t chan_read(const mb_slot_t *sl, uint8_t *buf, uint16_t max)
{
    if (sl->t != NULL) {
        return sl->t->read(buf, max, 0, sl->t->ctx);
    }
    return (int32_t)sl->sp->read(sl->sp->ctx, buf, max);
}

/* Length of an RTU frame from its first bytes: 0 = not known (yet). A Slave channel sees
 * REQUESTS (FC01..06 = 8 bytes, FC15/16 = 9 + byte count at byte 6); a Master channel sees
 * RESPONSES (exception = 5, FC01..04 = 5 + byte count at byte 2, FC05/06/15/16 = 8). */
static bool fr_track(mb_slot_t *sl, uint8_t b)
{
    if (sl->fr_total < sizeof sl->fr_hdr) { sl->fr_hdr[sl->fr_total] = b; }
    if (sl->fr_total < 0xFFFFU) { sl->fr_total++; }
    sl->fr_last_ms = sx_get_tick_ms();
#if MB_PORT_RESP_FRAMING
    if (sl->fr_resp) {
        if (sl->fr_total == 2U) {
            const uint8_t fc = sl->fr_hdr[1];
            if ((fc & 0x80U) != 0U)                                       { sl->fr_need = 5U; }
            else if (fc == 5U || fc == 6U || fc == 15U || fc == 16U)      { sl->fr_need = 8U; }
            else                                                          { sl->fr_need = 0U; } /* FC01..04: wait for the count; others: T3.5 */
        } else if (sl->fr_total == 3U && sl->fr_hdr[1] >= 1U && sl->fr_hdr[1] <= 4U) {
            sl->fr_need = (uint16_t)(5U + sl->fr_hdr[2]);                 /* uint8 count: at most 260 */
        }
        return sl->fr_need != 0U && sl->fr_total == sl->fr_need;
    }
#endif
    if (sl->fr_total == 2U) {
        const uint8_t fc = sl->fr_hdr[1];
        sl->fr_need = (fc >= 1U && fc <= 6U) ? 8U : 0U;
    } else if (sl->fr_total == 7U && (sl->fr_hdr[1] == 15U || sl->fr_hdr[1] == 16U)) {
        const uint16_t need = (uint16_t)(9U + sl->fr_hdr[6]);
        sl->fr_need = (need <= 256U) ? need : 0U;
    }
    return sl->fr_need != 0U && sl->fr_total == sl->fr_need;
}

/* How long a frame may stall half-way on a pipe that does not keep byte timing (same as the 1 s
 * character timeout of ASCII). After that the normal T3.5 rule ends it. */
#define MB_PORT_FR_STALL_MS 1000U

/* true while a request of this slot has started but its known length is not complete yet. */
static bool fr_waiting(const mb_slot_t *sl)
{
    if (!sl->fr_len || sl->fr_total == 0U || sl->fr_hold) { return false; }
    if ((uint32_t)(sx_get_tick_ms() - sl->fr_last_ms) >= MB_PORT_FR_STALL_MS) { return false; }
    if (sl->fr_total < 2U) { return true; }                                   /* FC not seen yet */
    if (sl->fr_need != 0U) { return sl->fr_total < sl->fr_need; }
#if MB_PORT_RESP_FRAMING
    if (sl->fr_resp) { return sl->fr_hdr[1] >= 1U && sl->fr_hdr[1] <= 4U && sl->fr_total < 3U; } /* byte count not seen */
#endif
    return (sl->fr_hdr[1] == 15U || sl->fr_hdr[1] == 16U) && sl->fr_total < 7U; /* byte count not seen */
}

/* Timers of one slot: while a request is still arriving by length, T3.5 is held back (the pause
 * is the pipe's, not the end of the frame). */
static void slot_timers(mb_slot_t *sl)
{
    if (fr_waiting(sl)) {
        const uint32_t now = sx_get_tick_ms();
        for (uint32_t i = 0; i < MB_PORT_MAX_TIMERS; i++) {
            mb_timer_t *tm = &s_timer[i];
            if (tm->used && tm->armed && tm->hdl == sl->lib_hdl) { tm->start_ms = now; }
        }
    }
    timers_run(sl->lib_hdl);
}

/* Make the library's T3.5 timer of this slot expire at once: the frame is complete. */
static void frame_end_now(mb_slot_t *sl)
{
    const uint32_t now = sx_get_tick_ms();
    for (uint32_t i = 0; i < MB_PORT_MAX_TIMERS; i++) {
        mb_timer_t *tm = &s_timer[i];
        if (tm->used && tm->armed && tm->hdl == sl->lib_hdl) { tm->start_ms = now - (uint32_t)tm->timeout_ms; }
    }
}

static void rx_feed(mb_slot_t *sl)
{
    /* The library turns its receiver off as soon as it has a whole frame (ASCII: at the LF). A
     * byte stream (a socket) can carry the NEXT request in the same read, so what the library
     * did not take stays in rx_pend[] and is offered again when the receiver is back on --
     * dropping it would lose that request. */
    for (uint32_t it = 0; it < MB_PORT_RX_MAX_ITER; it++) {
        if (sl->rx_cb == NULL || sl->fr_hold) {
            return;                      /* leave the bytes where they are until RX is on again */
        }
        if (sl->rx_pend_i >= sl->rx_pend_n) {
            const int32_t n = chan_read(sl, sl->rx_pend, (uint16_t)sizeof sl->rx_pend);
            sl->rx_pend_n = sl->rx_pend_i = 0U;
            if (n <= 0) {
                return;
            }
            sl->rx_pend_n = (uint8_t)n;
        }
        while (sl->rx_pend_i < sl->rx_pend_n && sl->rx_cb != NULL && !sl->fr_hold) {
            const uint8_t b = sl->rx_pend[sl->rx_pend_i++];
            sl->rx_cb(sl->lib_hdl, b);
            if (sl->fr_len && fr_track(sl, b)) {
                frame_end_now(sl);       /* whole request in: do not wait for T3.5 ... */
                sl->fr_hold = true;      /* ... and do not glue the next request onto it */
            }
        }
        if (sl->rx_pend_i < sl->rx_pend_n || sl->rx_pend_n < sizeof sl->rx_pend) {
            return;                      /* kept for the next tick / the pipe had no more */
        }
    }
}

/* RS-485 transmit of one frame (already in sl->sp_tx): blocking write() (returns after
 * the last bit; the echo of a transceiver that keeps RX open is then dropped) or
 * asynchronous write_start()/tx_done() (the port keeps the echo out itself and
 * ser_progress() ends the transmit). */
static void ser_send(mb_slot_t *sl, uint16_t n)
{
    const modbus_serial_port_t *p = sl->sp;
#if SPLC_MBM_ENABLE
    if (p->write_start != NULL && p->tx_done != NULL) {
        if (p->set_tx != NULL) { p->set_tx(p->ctx, true); }
        if (p->write_start(p->ctx, sl->sp_tx, n) != 0) {
            if (p->set_tx != NULL) { p->set_tx(p->ctx, false); }
            log_warn(TAG, "rs485 write_start refused (%u bytes)", (unsigned)n);
            return;
        }
        uint32_t ms = 50U;
        if (sl->sp_baud != 0U) { ms = 2U * (((uint32_t)n * 11000U) / sl->sp_baud) + 50U; }
        sl->sp_async    = true;
        sl->sp_async_dl = sx_get_tick_ms() + ms;
        return;
    }
#endif
    if (p->write == NULL) {
        log_error(TAG, "rs485 port has neither write() nor write_start()/tx_done()");
        return;
    }
    uint32_t tx_ms = 50U;
    if (sl->sp_baud != 0U) {
        const uint32_t need = ((uint32_t)n * 11000U) / sl->sp_baud + 20U;
        if (need > tx_ms) { tx_ms = need; }
    }
    if (p->set_tx != NULL) { p->set_tx(p->ctx, true); }
    const int rc = p->write(p->ctx, sl->sp_tx, n, tx_ms);
    if (p->set_tx != NULL) { p->set_tx(p->ctx, false); }
    uint8_t scratch[16];
    for (uint32_t i = 0; i < MB_PORT_ECHO_DRAIN; i++) {   /* echo of our own frame */
        if (p->read(p->ctx, scratch, (uint16_t)sizeof scratch) <= 0) { break; }
    }
    if (rc != 0) { log_warn(TAG, "rs485 write failed (%d), %u bytes", rc, (unsigned)n); }
}

/* Ends an asynchronous transmit (direction back to receive) once tx_done(), or gives up. */
static void ser_progress(mb_slot_t *sl)
{
#if SPLC_MBM_ENABLE
    if (!sl->sp_async) { return; }
    const modbus_serial_port_t *p = sl->sp;
    if (p->tx_done(p->ctx)) {
        sl->sp_async = false;
        if (p->set_tx != NULL) { p->set_tx(p->ctx, false); }
    } else if ((int32_t)(sx_get_tick_ms() - sl->sp_async_dl) >= 0) {
        sl->sp_async = false;
        if (p->write_abort != NULL) { p->write_abort(p->ctx); }
        if (p->set_tx != NULL) { p->set_tx(p->ctx, false); }
        log_warn(TAG, "rs485 transmit never completed, aborted");
    }
#else
    (void)sl;      /* the port has blocking write() only */
#endif
}

static void tx_flush(mb_slot_t *sl)
{
    if (!sl->tx_pending || sl->tx_cb == NULL) { return; }
    sl->tx_pending = false;
    const pbMBPSerialTransmitterEmptyCB cb = sl->tx_cb;
    const bool      ser = (sl->sp != NULL);
    uint8_t        *buf = ser ? sl->sp_tx : s_tx_buf;
    const uint32_t  cap = ser ? MB_PORT_SER_TX_MAX : MB_PORT_TX_MAX;
    uint32_t n = 0;
    UBYTE    b = 0;
    /* The library hands out one byte per call and returns FALSE after the last
     * one (it re-enables its receiver on that call). A length-0 "dummy
     * transmission" returns FALSE at once: nothing is written. */
    while (n < cap && cb(sl->lib_hdl, &b)) { buf[n++] = b; }
    if (n >= cap) {
        log_error(TAG, "frame longer than %u bytes dropped", (unsigned)cap);
        return;
    }
    if (n == 0U) { return; }
    if (ser) { ser_send(sl, (uint16_t)n); return; }
    const int32_t w = sl->t->write(buf, (uint16_t)n, MB_PORT_WRITE_TIMEOUT_MS, sl->t->ctx);
    if (w != (int32_t)n) {
        log_warn(TAG, "reply write: %ld of %u bytes", (long)w, (unsigned)n);
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
#if SPLC_MODBUS_SLAVE_ENABLE
        (void)eMBSRegisterCoilCB(tl->server, applink_coil_tramp);
        (void)eMBSRegisterDiscreteCB(tl->server, applink_disc_tramp);
#endif
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
    sl->fr_len    = (mode == MB_RTU) && (t->frame_sync == SPLC_MODBUS_SYNC_LENGTH);
#if MB_PORT_RESP_FRAMING
    sl->fr_resp   = false;
#endif
    sl->fr_hold   = false;
    sl->fr_total  = sl->fr_need = 0U;
    sl->rx_pend_n = sl->rx_pend_i = 0U;

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
#if SPLC_MODBUS_SLAVE_ENABLE
    (void)eMBSRegisterCoilCB(sl->server, applink_coil_tramp);
    (void)eMBSRegisterDiscreteCB(sl->server, applink_disc_tramp);
#endif
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
    slot_timers(sl);                /* 3. ... then timers (order matters: 86)   */
    (void)eMBSPoll(sl->server);     /* 4. MBS_POLL_SINGLE_CYCLE=1: runs to idle */
    tx_flush(sl);                   /* 5. reply out, library re-enables RX      */
    sl->fr_hold = false;            /* the request is answered (or ignored): the next may come in */
}


/* --- RS-485 channels: Modbus SLAVE (plc_mbs) ------------------------------ */

#if SPLC_MODBUS_SLAVE_ENABLE
int mb_port_slave_open(const modbus_serial_port_t *port, uint32_t baud, uint8_t slave_id,
                       uint8_t framing, const mb_port_slave_ops_t *ops)
{
    if (port == NULL || port->read == NULL || ops == NULL || ops->holding == NULL ||
        ops->coils == NULL || ops->discrete == NULL || slave_id < 1U || slave_id > 247U) {
        log_error(TAG, "slave open: bad argument (id %u)", (unsigned)slave_id);
        return -1;
    }
    const int ch = mb_port_serial_attach(port, baud);
    if (ch < 0) { return -1; }
    mb_slot_t *sl = &s_slot[ch];
    s_slave_ops = *ops;
    const eMBSerialMode mode = (framing == SPLC_MODBUS_FRAMING_ASCII) ? MB_ASCII : MB_RTU;
    sl->fr_len  = (mode == MB_RTU) && (port->frame_sync == SPLC_MODBUS_SYNC_LENGTH);
#if MB_PORT_RESP_FRAMING
    sl->fr_resp = false;
#endif
    eMBErrorCode e = eMBSSerialInit(&sl->server, mode, slave_id, (UBYTE)ch,
                                    (baud != 0U) ? baud : 9600UL, MB_PAR_NONE);
    if (e != MB_ENOERR) {
        log_error(TAG, "slave: eMBSSerialInit failed: err=%d id=%u %s", (int)e, (unsigned)slave_id,
                  (mode == MB_ASCII) ? "ASCII" : "RTU");
        mb_port_serial_detach(ch);
        return -1;
    }
    if (eMBSRegisterHoldingCB(sl->server, slave_holding_tramp) != MB_ENOERR ||
        eMBSRegisterCoilCB(sl->server, slave_coil_tramp) != MB_ENOERR ||
        eMBSRegisterDiscreteCB(sl->server, slave_disc_tramp) != MB_ENOERR) {
        log_error(TAG, "slave: callback registration failed");
        (void)eMBSClose(sl->server);
        mb_port_serial_detach(ch);
        return -1;
    }
    sl->created = true;
    return ch;
}

void mb_port_slave_poll(int ch)
{
    if (ch < (int)MB_PORT_SLOTS || ch >= (int)MB_PORT_SER_END) { return; }
    mb_slot_t *sl = &s_slot[ch];
    if (sl->sp == NULL || !sl->created || !sl->open) { return; }
    ser_progress(sl);               /* end a transmit started earlier                */
    rx_feed(sl);                    /* bytes first ...  (order matters: 86)          */
    slot_timers(sl);                /* ... then this channel's timers (T3.5)         */
    (void)eMBSPoll(sl->server);     /* MBS_POLL_SINGLE_CYCLE=1: runs to idle         */
    tx_flush(sl);                   /* reply out, library re-enables RX              */
    sl->fr_hold = false;            /* the request is answered (or ignored): the next may come in */
}

void mb_port_slave_close(int ch)
{
    if (ch < (int)MB_PORT_SLOTS || ch >= (int)MB_PORT_SER_END) { return; }
    mb_slot_t *sl = &s_slot[ch];
    if (sl->sp == NULL) { return; }
    if (sl->created) { (void)eMBSClose(sl->server); }   /* frees the library's timer + event */
    mb_port_serial_detach(ch);                           /* aborts a transmit in flight       */
}
#else
int  mb_port_slave_open(const modbus_serial_port_t *p, uint32_t b, uint8_t id, uint8_t f, const mb_port_slave_ops_t *o)
{ (void)p; (void)b; (void)id; (void)f; (void)o; return -1; }
void mb_port_slave_poll(int ch)  { (void)ch; }
void mb_port_slave_close(int ch) { (void)ch; }
#endif

/* --- RS-485 channels (Modbus master over a modbus_serial_port_t) ---------- */

int mb_port_serial_attach(const modbus_serial_port_t *port, uint32_t baud)
{
    if (port == NULL || port->read == NULL) { return -1; }
    for (uint32_t i = MB_PORT_SLOTS; i < MB_PORT_SER_END; i++) {
        mb_slot_t *sl = &s_slot[i];
        if (sl->t == NULL && sl->sp == NULL && !sl->open) {
            memset(sl, 0, sizeof *sl);
            sl->sp      = port;
            sl->sp_baud = baud;
            /* A pipe that does not keep byte timing (Zigbee): a MASTER channel cuts the RESPONSE
             * by its length; mb_port_slave_open() overrides both fields for a Slave. */
#if MB_PORT_RESP_FRAMING
            sl->fr_len  = (port->frame_sync == SPLC_MODBUS_SYNC_LENGTH);
            sl->fr_resp = true;
#endif
            return (int)i;
        }
    }
    log_error(TAG, "no free RS-485 channel (%u)", (unsigned)MB_PORT_RS485_CH);
    return -1;
}

void mb_port_serial_set_baud(int ch, uint32_t baud)
{
    if (ch >= (int)MB_PORT_SLOTS && ch < (int)MB_PORT_CHANNELS) { s_slot[ch].sp_baud = baud; }
}

void mb_port_serial_detach(int ch)
{
    if (ch < (int)MB_PORT_SLOTS || ch >= (int)MB_PORT_SER_END) { return; }
    mb_slot_t *sl = &s_slot[ch];
#if SPLC_MBM_ENABLE
    if (sl->sp != NULL && sl->sp_async && sl->sp->write_abort != NULL) {
        sl->sp->write_abort(sl->sp->ctx);
    }
#endif
    memset(sl, 0, sizeof *sl);
}

void mb_port_serial_pump(int ch)
{
    if (ch < (int)MB_PORT_SLOTS || ch >= (int)MB_PORT_SER_END) { return; }
    mb_slot_t *sl = &s_slot[ch];
    if (sl->sp == NULL || !sl->open) { return; }
    ser_progress(sl);
    rx_feed(sl);                    /* bytes first ...  (order matters: 86) */
#if MB_PORT_RESP_FRAMING
    slot_timers(sl);                /* ... then this channel's timers (T3.5 held while a length-framed response arrives) */
#else
    timers_run(sl->lib_hdl);        /* ... then this channel's timers       */
#endif
}

void mb_port_serial_flush(int ch)
{
    if (ch < (int)MB_PORT_SLOTS || ch >= (int)MB_PORT_SER_END) { return; }
    mb_slot_t *sl = &s_slot[ch];
    if (sl->sp != NULL && sl->open) { tx_flush(sl); }
#if MB_PORT_RESP_FRAMING
    if (sl->sp != NULL) { sl->fr_hold = false; }   /* the response is handled: the next frame may come in */
#endif
}

void mb_port_serial_drain(int ch)
{
    if (ch < (int)MB_PORT_SLOTS || ch >= (int)MB_PORT_SER_END || s_slot[ch].sp == NULL) { return; }
    s_slot[ch].rx_pend_n = s_slot[ch].rx_pend_i = 0U;       /* stale bytes already read belong to nobody */
    uint8_t scratch[16];
    for (uint32_t i = 0; i < MB_PORT_ECHO_DRAIN; i++) {
        if (s_slot[ch].sp->read(s_slot[ch].sp->ctx, scratch, (uint16_t)sizeof scratch) <= 0) { break; }
    }
}

/* --- TCP master channels (host-owned sockets) ----------------------------- */
#if SPLC_MODBUS_TCP_MASTER_ENABLE
static bool tcpc_valid(int ch) { return ch >= (int)MB_PORT_SER_END && ch < (int)MB_PORT_CHANNELS; }

int mb_port_tcpc_attach(const mb_port_tcpc_ops_t *ops)
{
    if (ops == NULL || ops->read == NULL || ops->write == NULL || ops->close == NULL) { return -1; }
    for (uint32_t i = MB_PORT_SER_END; i < MB_PORT_CHANNELS; i++) {
        mb_slot_t *sl = &s_slot[i];
        if (sl->tc == NULL && !sl->tcpc) {
            memset(sl, 0, sizeof *sl);
            sl->tc = ops;
            return (int)i;
        }
    }
    log_error(TAG, "no free TCP master channel (%u)", (unsigned)MB_PORT_TCPM_CH);
    return -1;
}

void mb_port_tcpc_detach(int ch)
{
    if (!tcpc_valid(ch)) { return; }
    mb_slot_t *sl = &s_slot[ch];
    if (sl->tc != NULL && sl->tcp_up) { sl->tc->close(sl->tc->ctx); }
    memset(sl, 0, sizeof *sl);
}

void mb_port_tcpc_bind(int ch) { s_tcpc_pending = ch; }

void mb_port_tcpc_pump(int ch)
{
    if (!tcpc_valid(ch)) { return; }
    mb_slot_t *sl = &s_slot[ch];
    if (!sl->tcpc) { return; }
    for (uint32_t it = 0; it < MB_PORT_TCP_FEED_ITER && sl->tcp_up; it++) {   /* bytes first ... (86) */
        sl->tcp_got = 0U;
        (void)sl->tcp_newdata(sl->lib_hdl, (xMBPTCPClientHandle)&sl->tcp_up);
        if (sl->tcp_got == 0U) { break; }     /* pipe empty, or the library wants no more */
    }
    timers_run(sl->lib_hdl);                  /* ... then this channel's timers (answer timeout) */
}

bool mb_port_tcpc_is_up(int ch)
{
    return tcpc_valid(ch) && s_slot[ch].tcpc && s_slot[ch].tcp_up && !s_slot[ch].tcpc_lost;
}

bool mb_port_tcpc_take_lost(int ch)
{
    if (!tcpc_valid(ch)) { return false; }
    const bool lost = s_slot[ch].tcpc_lost;
    s_slot[ch].tcpc_lost = false;
    return lost;
}
#else
int  mb_port_tcpc_attach(const mb_port_tcpc_ops_t *ops) { (void)ops; return -1; }
void mb_port_tcpc_detach(int ch)                        { (void)ch; }
void mb_port_tcpc_bind(int ch)                          { (void)ch; }
void mb_port_tcpc_pump(int ch)                          { (void)ch; }
bool mb_port_tcpc_is_up(int ch)                         { (void)ch; return false; }
bool mb_port_tcpc_take_lost(int ch)                     { (void)ch; return false; }
#endif

#endif /* SPLC_MODBUS_LIB == SPLC_MODBUS_LIB_MB */