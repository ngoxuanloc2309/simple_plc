/*
 * modbus_tcp_master.c - the modbus_tcp_master.h API on libs/mb_master_slave (mbmaster/tcp).
 * Empty unless SPLC_MODBUS_LIB == MB and SPLC_MODBUS_TCP_MASTER_ENABLE == 1.
 *
 * A request: start() fills the request, connects through the host if needed (one step per
 * scan) and kicks the library's POLLED state machine; poll() drives it on later scans.
 * One drive step, in the order handoff 86 requires: pump (RX bytes into the library, then its
 * soft timers) -> library (vMBM...Polled until the state stops changing). The frame leaves
 * from inside the library via mb_port's eMBPTCPConWrite() -> host write().
 *
 * Versus the RTU master (modbus_serial_mb.c): the library checks the MBAP transaction id (a
 * late answer to an earlier request is ignored); a wrong unit / function / length is ERR_FRAME;
 * no inter-frame gap; ERR_CONNECT is new.
 */
#include "modbus_tcp_master.h"

#if (SPLC_MODBUS_LIB == SPLC_MODBUS_LIB_MB) && SPLC_MODBUS_TCP_MASTER_ENABLE

#include <string.h>

#include "mbport.h"
#include "mbm.h"
#include "common/mbframe.h"
#include "common/mbutils.h"

#include "mb_port.h"
#include "sx_time.h"
#include "splc_log.h"

static const char *TAG = "MB_TCPM";

#define FC_READ_COILS      0x01U
#define FC_READ_DISCRETE   0x02U
#define FC_READ_HOLDING    0x03U
#define FC_READ_INPUT      0x04U
#define FC_WRITE_COIL      0x05U
#define FC_WRITE_REG       0x06U
#define FC_WRITE_COILS     0x0FU
#define FC_WRITE_MULTIPLE  0x10U

#define MB_STEP_MAX        6U    /* library states walked in one drive step */
#define TCPM_TIMEOUT_DROP  3U    /* consecutive answer timeouts that drop the connection */
#define TCPM_MAX           (SPLC_MODBUS_TCP_MASTER_COUNT)

enum { CONN_DOWN = 0, CONN_CONNECTING, CONN_UP };
enum { PH_NONE = 0, PH_CONNECT, PH_EXCHANGE };

/* Masters that went through init, so a second init of the same object releases its channel
 * first (never trust the bytes of an uninitialised struct). */
static modbus_tcp_master_t *s_reg[TCPM_MAX];
static mb_port_tcpc_ops_t   s_ops[TCPM_MAX];

static int reg_index(const modbus_tcp_master_t *m)
{
    for (uint32_t i = 0; i < TCPM_MAX; i++) { if (s_reg[i] == m) { return (int)i; } }
    return -1;
}

/* the channel's view of the host callbacks */
static int  t_read(void *c, uint8_t *b, uint16_t n)        { modbus_tcp_master_t *m = c; return m->port.read(m->port.ctx, b, n); }
static int  t_write(void *c, const uint8_t *b, uint16_t n) { modbus_tcp_master_t *m = c; return m->port.write(m->port.ctx, b, n); }
static void t_close(void *c)                               { modbus_tcp_master_t *m = c; m->port.close(m->port.ctx); }

/* --- connection ----------------------------------------------------------- */

/* Close library client + host socket without touching the request state. */
static void drop_connection(modbus_tcp_master_t *m)
{
    if (m->mb_hdl != NULL) {
        (void)eMBMTCPDisconnect((xMBMHandle)m->mb_hdl);   /* -> ConClose -> host close() if the client was up */
    }
    if (m->conn != CONN_DOWN && m->port.close != NULL) {
        m->port.close(m->port.ctx);                       /* connecting / not yet the library's; idempotent */
    }
    /* The library instance goes too: after a failed send / a dropped client it can still hold an
     * armed answer timer or a queued event of the dead request, which would fail the NEXT
     * request (seen on the host harness after a write failure). It is re-created on demand. */
    if (m->mb_hdl != NULL) { (void)eMBMClose((xMBMHandle)m->mb_hdl); m->mb_hdl = NULL; }
    if (m->mb_ch >= 0) { (void)mb_port_tcpc_take_lost(m->mb_ch); }
    m->conn     = CONN_DOWN;
    m->timeouts = 0U;
}

static void release(modbus_tcp_master_t *m)
{
    drop_connection(m);
    if (m->mb_ch >= 0)     { mb_port_tcpc_detach(m->mb_ch); m->mb_ch = -1; }
    const int i = reg_index(m);
    if (i >= 0) { s_reg[i] = NULL; memset(&s_ops[i], 0, sizeof s_ops[i]); }
}

static bool ensure_instance(modbus_tcp_master_t *m)
{
    if (m->mb_hdl != NULL) { return true; }
    xMBMHandle h = NULL;
    mb_port_tcpc_bind(m->mb_ch);                          /* eMBPTCPClientInit() has no channel argument */
    const eMBErrorCode e = eMBMTCPInit(&h);
    mb_port_tcpc_bind(-1);
    if (e != MB_ENOERR) {
        log_error(TAG, "eMBMTCPInit failed: err=%d ch=%d", (int)e, m->mb_ch);
        return false;
    }
    m->mb_hdl = h;
    return true;
}

/* --- request bookkeeping -------------------------------------------------- */

static void finish(modbus_tcp_master_t *m, modbus_master_state_t st, modbus_master_error_t err)
{
    m->state          = st;
    m->error          = err;
    m->phase          = PH_NONE;
    m->mb_qstate      = (uint8_t)MBM_STATE_NONE;
    m->quiet_until_ms = sx_get_tick_ms();
}

static void connect_failed(modbus_tcp_master_t *m)
{
    finish(m, MODBUS_MASTER_DONE_ERR, MODBUS_MASTER_ERR_CONNECT);
    m->quiet_until_ms = sx_get_tick_ms() + m->retry_ms;   /* no hammering of a dead host */
}

static modbus_master_state_t take_result(modbus_tcp_master_t *m)
{
    const modbus_master_state_t r = m->state;
    m->state = MODBUS_MASTER_IDLE;
    return r;
}

static bool unit_ok(uint8_t unit) { return (unit >= 1U && unit <= 247U) || unit == 255U; }

static bool can_start(const modbus_tcp_master_t *m, uint8_t unit)
{
    if (m->mb_ch < 0 || m->state != MODBUS_MASTER_IDLE || m->tcp_port == 0U || !unit_ok(unit)) {
        return false;
    }
    return (int32_t)(sx_get_tick_ms() - m->quiet_until_ms) >= 0;
}

/* --- result mapping ------------------------------------------------------- */

static void complete(modbus_tcp_master_t *m, eMBErrorCode e)
{
    if (e == MB_ENOERR) {
        if (m->func == FC_READ_COILS || m->func == FC_READ_DISCRETE) {
            for (uint16_t i = 0U; i < m->qty; i++) {      /* LSB first */
                m->regs[i] = (uint16_t)((m->mb_bits[i >> 3] >> (i & 7U)) & 1U);
            }
        }
        m->timeouts = 0U;
        finish(m, MODBUS_MASTER_DONE_OK, MODBUS_MASTER_ERR_NONE);
        return;
    }
    /* A broken connection beats whatever error code the library chose for it. */
    const bool lost = mb_port_tcpc_take_lost(m->mb_ch) || !mb_port_tcpc_is_up(m->mb_ch);
    if (lost) {
        log_warn(TAG, "ch %d: connection lost (lib err=%d)", m->mb_ch, (int)e);
        drop_connection(m);
        connect_failed(m);                                /* ERR_CONNECT + retry pause */
        return;
    }
    modbus_master_error_t err;
    if (e == MB_ETIMEDOUT) {
        err = MODBUS_MASTER_ERR_TIMEOUT;
        if (++m->timeouts >= TCPM_TIMEOUT_DROP) {         /* a half-open socket never answers again */
            log_warn(TAG, "ch %d: %u timeouts in a row, dropping the connection", m->mb_ch, (unsigned)m->timeouts);
            drop_connection(m);
        }
    } else if (e >= MB_EX_ILLEGAL_FUNCTION && e <= MB_EX_GATEWAY_TARGET_FAILED) {
        err = MODBUS_MASTER_ERR_EXCEPTION;
        m->exception_code = (uint8_t)eMBErrorcodeToException(e);
        m->timeouts = 0U;
    } else {
        err = MODBUS_MASTER_ERR_FRAME;                    /* wrong unit / function / length / echo */
        m->timeouts = 0U;
    }
    finish(m, MODBUS_MASTER_DONE_ERR, err);
}

static void lib_step(modbus_tcp_master_t *m, eMBMQueryState *st, eMBErrorCode *err)
{
    xMBMHandle h = (xMBMHandle)m->mb_hdl;
    switch (m->func) {
    case FC_READ_COILS:     vMBMReadCoilsPolled(h, m->unit, m->addr, m->qty, m->mb_bits, st, err); break;
    case FC_READ_DISCRETE:  vMBMReadDiscreteInputsPolled(h, m->unit, m->addr, m->qty, m->mb_bits, st, err); break;
    case FC_READ_HOLDING:   vMBMReadHoldingRegistersPolled(h, m->unit, m->addr, (UBYTE)m->qty, m->regs, st, err); break;
    case FC_READ_INPUT:     vMBMReadInputRegistersPolled(h, m->unit, m->addr, (UBYTE)m->qty, m->regs, st, err); break;
    case FC_WRITE_COIL:     vMBMWriteSingleCoilPolled(h, m->unit, m->addr, (m->regs[0] != 0U) ? TRUE : FALSE, st, err); break;
    case FC_WRITE_REG:      vMBMWriteSingleRegisterPolled(h, m->unit, m->addr, m->regs[0], st, err); break;
    case FC_WRITE_COILS:    vMBMWriteCoilsPolled(h, m->unit, m->addr, m->qty, m->mb_bits, st, err); break;
    case FC_WRITE_MULTIPLE: vMBMWriteMultipleRegistersPolled(h, m->unit, m->addr, (UBYTE)m->qty, m->regs, st, err); break;
    default:                *st = MBM_STATE_DONE; *err = MB_EINVAL; break;
    }
}

static void drive(modbus_tcp_master_t *m)
{
    mb_port_tcpc_pump(m->mb_ch);                          /* 1. bytes, then timers */
    eMBMQueryState st  = (eMBMQueryState)m->mb_qstate;
    eMBErrorCode   err = MB_ENOERR;
    for (unsigned i = 0U; i < MB_STEP_MAX; i++) {         /* 2. library (the frame goes out in here) */
        const eMBMQueryState before = st;
        lib_step(m, &st, &err);
        if (st == MBM_STATE_DONE || st == before) { break; }
    }
    m->mb_qstate = (uint8_t)st;
    if (st == MBM_STATE_DONE) { complete(m, err); }
}

static void begin_exchange(modbus_tcp_master_t *m)
{
    uint32_t t = m->timeout_ms;
    if (t > 65535U) { t = 65535U; }
    (void)eMBMSetSlaveTimeout((xMBMHandle)m->mb_hdl, (uint16_t)t);
    m->phase     = PH_EXCHANGE;
    m->mb_qstate = (uint8_t)MBM_STATE_NONE;
    drive(m);                                             /* frame out now, like the RTU master */
}

/* One step of the connect phase: ask the host until it says connected / failed / out of time. */
static void advance_connect(modbus_tcp_master_t *m)
{
    const int rc = m->port.connect(m->port.ctx, m->host, m->tcp_port);
    if (m->conn == CONN_DOWN) { m->conn = CONN_CONNECTING; }
    if (rc == 0) {
        if (!ensure_instance(m)) {                        /* a drop discarded it */
            drop_connection(m);
            connect_failed(m);
            return;
        }
        const eMBErrorCode e = eMBMTCPConnect((xMBMHandle)m->mb_hdl, m->host, m->tcp_port);
        if (e != MB_ENOERR) {
            log_error(TAG, "ch %d: eMBMTCPConnect failed: err=%d", m->mb_ch, (int)e);
            drop_connection(m);
            connect_failed(m);
            return;
        }
        m->conn     = CONN_UP;
        m->timeouts = 0U;
        log_info(TAG, "ch %d: connected to %s:%u", m->mb_ch, m->host, (unsigned)m->tcp_port);
        begin_exchange(m);
    } else if (rc > 0) {
        if ((int32_t)(sx_get_tick_ms() - m->conn_deadline_ms) >= 0) {
            log_warn(TAG, "ch %d: connect to %s:%u timed out", m->mb_ch, m->host, (unsigned)m->tcp_port);
            drop_connection(m);
            connect_failed(m);
        }
    } else {
        log_warn(TAG, "ch %d: connect to %s:%u failed (%d)", m->mb_ch, m->host, (unsigned)m->tcp_port, rc);
        drop_connection(m);
        connect_failed(m);
    }
}

static bool start(modbus_tcp_master_t *m, uint32_t timeout_ms)
{
    if (!ensure_instance(m)) { return false; }
    m->timeout_ms     = timeout_ms;
    m->error          = MODBUS_MASTER_ERR_NONE;
    m->exception_code = 0U;
    m->mb_qstate      = (uint8_t)MBM_STATE_NONE;
    m->state          = MODBUS_MASTER_BUSY;
    if (m->conn == CONN_UP && !mb_port_tcpc_is_up(m->mb_ch)) {
        drop_connection(m);                               /* died while idle, nobody polled */
    }
    if (m->conn == CONN_UP) {
        begin_exchange(m);
    } else {
        m->phase            = PH_CONNECT;
        m->conn_deadline_ms = sx_get_tick_ms() + m->connect_ms;
        advance_connect(m);                               /* first connect step now */
    }
    return true;
}

/* --- public API ----------------------------------------------------------- */

bool modbus_tcp_master_init(modbus_tcp_master_t *m, const modbus_tcp_master_port_t *port)
{
    if (m == NULL) { return false; }
    if (reg_index(m) >= 0) { release(m); }                /* re-init of the same object */
    memset(m, 0, sizeof(*m));
    m->state      = MODBUS_MASTER_IDLE;
    m->mb_ch      = -1;
    m->connect_ms = (uint32_t)SPLC_MODBUS_TCP_MASTER_CONNECT_MS;
    m->retry_ms   = (uint32_t)SPLC_MODBUS_TCP_MASTER_RETRY_MS;
    if (port == NULL || port->connect == NULL || port->read == NULL ||
        port->write == NULL || port->close == NULL) {
        return false;                                     /* every request is refused */
    }
    int slot = -1;
    for (uint32_t i = 0; i < TCPM_MAX; i++) { if (s_reg[i] == NULL) { slot = (int)i; break; } }
    if (slot < 0) {
        log_error(TAG, "more TCP masters than SPLC_MODBUS_TCP_MASTER_COUNT (%u)", (unsigned)TCPM_MAX);
        return false;
    }
    m->port = *port;
    s_ops[slot].read  = t_read;
    s_ops[slot].write = t_write;
    s_ops[slot].close = t_close;
    s_ops[slot].ctx   = m;
    m->mb_ch = mb_port_tcpc_attach(&s_ops[slot]);
    if (m->mb_ch < 0) {
        memset(&s_ops[slot], 0, sizeof s_ops[slot]);
        memset(&m->port, 0, sizeof m->port);
        return false;
    }
    s_reg[slot] = m;
    return true;
}

void modbus_tcp_master_deinit(modbus_tcp_master_t *m)
{
    if (m == NULL || reg_index(m) < 0) { return; }
    release(m);
    m->state = MODBUS_MASTER_IDLE;
    memset(&m->port, 0, sizeof m->port);
}

bool modbus_tcp_master_set_target(modbus_tcp_master_t *m, const char *host, uint16_t tcp_port)
{
    if (m == NULL || host == NULL || tcp_port == 0U || m->state == MODBUS_MASTER_BUSY ||
        strlen(host) >= sizeof m->host) {
        return false;
    }
    if (m->conn != CONN_DOWN && (tcp_port != m->tcp_port || strcmp(host, m->host) != 0)) {
        drop_connection(m);                               /* it points at the old target */
    }
    memcpy(m->host, host, strlen(host) + 1U);
    m->tcp_port = tcp_port;
    return true;
}

void modbus_tcp_master_set_connect_timeout_ms(modbus_tcp_master_t *m, uint32_t ms)
{
    if (m == NULL) { return; }
    m->connect_ms = (ms < 100U) ? 100U : (ms > 60000U ? 60000U : ms);
}

void modbus_tcp_master_set_retry_ms(modbus_tcp_master_t *m, uint32_t ms)
{
    if (m == NULL) { return; }
    m->retry_ms = (ms > 60000U) ? 60000U : ms;
}

static bool read_common(modbus_tcp_master_t *m, uint8_t unit, uint8_t fc, uint16_t addr,
                        uint16_t qty, uint32_t timeout_ms)
{
    if (m == NULL || qty < 1U || qty > MODBUS_MASTER_MAX_REGS || !can_start(m, unit)) { return false; }
    m->unit = unit; m->func = fc; m->addr = addr; m->qty = qty;
    return start(m, timeout_ms);
}

bool modbus_tcp_master_read_fc(modbus_tcp_master_t *m, uint8_t unit, uint8_t fc, uint16_t addr,
                               uint16_t qty, uint32_t timeout_ms)
{
    if (fc != FC_READ_COILS && fc != FC_READ_DISCRETE && fc != FC_READ_HOLDING && fc != FC_READ_INPUT) {
        return false;
    }
    return read_common(m, unit, fc, addr, qty, timeout_ms);
}

bool modbus_tcp_master_read(modbus_tcp_master_t *m, uint8_t unit, uint16_t addr, uint16_t qty,
                            uint32_t timeout_ms)
{
    return read_common(m, unit, FC_READ_HOLDING, addr, qty, timeout_ms);
}

bool modbus_tcp_master_write(modbus_tcp_master_t *m, uint8_t unit, uint16_t addr,
                             const uint16_t *values, uint16_t qty, uint32_t timeout_ms)
{
    if (m == NULL || values == NULL || qty < 1U || qty > MODBUS_MASTER_MAX_WRITE_REGS ||
        !can_start(m, unit)) {
        return false;
    }
    for (uint16_t i = 0U; i < qty; i++) { m->regs[i] = values[i]; }
    m->unit = unit; m->func = FC_WRITE_MULTIPLE; m->addr = addr; m->qty = qty;
    return start(m, timeout_ms);
}

bool modbus_tcp_master_write_fc(modbus_tcp_master_t *m, uint8_t unit, uint8_t fc, uint16_t addr,
                                const uint16_t *values, uint16_t qty, uint32_t timeout_ms)
{
    if (m == NULL || values == NULL || qty < 1U) { return false; }
    if (fc == FC_WRITE_MULTIPLE) { return modbus_tcp_master_write(m, unit, addr, values, qty, timeout_ms); }
    if (!can_start(m, unit)) { return false; }
    if (fc == FC_WRITE_COIL || fc == FC_WRITE_REG) {
        if (qty != 1U) { return false; }
        m->regs[0] = (fc == FC_WRITE_COIL) ? ((values[0] != 0U) ? 1U : 0U) : values[0];
        m->unit = unit; m->func = fc; m->addr = addr; m->qty = 1U;
        return start(m, timeout_ms);
    }
    if (fc == FC_WRITE_COILS) {
        if (qty > (uint16_t)(8U * sizeof m->mb_bits)) { return false; }
        memset(m->mb_bits, 0, sizeof m->mb_bits);
        for (uint16_t i = 0U; i < qty; i++) {
            if (values[i] != 0U) { m->mb_bits[i >> 3] |= (uint8_t)(1U << (i & 7U)); }   /* LSB first */
        }
        m->unit = unit; m->func = fc; m->addr = addr; m->qty = qty;
        return start(m, timeout_ms);
    }
    return false;
}

modbus_master_state_t modbus_tcp_master_poll(modbus_tcp_master_t *m)
{
    if (m == NULL) { return MODBUS_MASTER_IDLE; }
    if (m->state == MODBUS_MASTER_DONE_OK || m->state == MODBUS_MASTER_DONE_ERR) { return take_result(m); }
    if (m->state != MODBUS_MASTER_BUSY) {
        if (m->conn == CONN_UP && m->mb_ch >= 0) {        /* idle: watch the socket */
            mb_port_tcpc_pump(m->mb_ch);                  /* drops stray bytes, notices a closed peer */
            if (mb_port_tcpc_take_lost(m->mb_ch) || !mb_port_tcpc_is_up(m->mb_ch)) {
                log_info(TAG, "ch %d: connection closed while idle", m->mb_ch);
                drop_connection(m);
            }
        }
        return m->state;
    }
    if (m->phase == PH_CONNECT) { advance_connect(m); } else { drive(m); }
    if (m->state == MODBUS_MASTER_DONE_OK || m->state == MODBUS_MASTER_DONE_ERR) { return take_result(m); }
    return MODBUS_MASTER_BUSY;
}

const uint16_t *modbus_tcp_master_regs(const modbus_tcp_master_t *m)        { return m->regs; }
modbus_master_error_t modbus_tcp_master_error(const modbus_tcp_master_t *m) { return m->error; }
uint8_t modbus_tcp_master_exception(const modbus_tcp_master_t *m)           { return m->exception_code; }
bool modbus_tcp_master_is_busy(const modbus_tcp_master_t *m)                { return m->state == MODBUS_MASTER_BUSY; }
bool modbus_tcp_master_is_connected(const modbus_tcp_master_t *m)
{
    return m->conn == CONN_UP && m->mb_ch >= 0 && mb_port_tcpc_is_up(m->mb_ch);
}
void modbus_tcp_master_disconnect(modbus_tcp_master_t *m)
{
    if (m == NULL || m->state == MODBUS_MASTER_BUSY || m->conn == CONN_DOWN) { return; }
    drop_connection(m);
}

#endif /* LIB == MB && SPLC_MODBUS_TCP_MASTER_ENABLE */