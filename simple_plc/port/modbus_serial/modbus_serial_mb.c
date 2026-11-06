/*
 * modbus_serial_mb.c - the modbus_serial.h master API on libs/mb_master_slave
 * (SPLC_MODBUS_LIB == SPLC_MODBUS_LIB_MB). Replaces modbus_serial.c for that
 * library choice; plc_mbm, plc_mbw and plc_io_remote call exactly the same
 * functions and see the same states, errors and limits.
 *
 * How a request runs: modbus_rtu_master_read*()/write*() fill in the request and
 * kick the library's POLLED state machine once; modbus_rtu_master_poll() drives
 * it again on every scan. One drive step, in the order handoff 86 requires:
 *   1. pump   : RX bytes into the library, then the channel's soft timers (T3.5,
 *               answer timeout)
 *   2. library: vMBM...Polled() until its state stops changing
 *   3. flush  : the frame the library prepared goes out (blocking write() or the
 *               write_start()/tx_done() pair of the port)
 * Never blocks beyond the transmit of one frame (same as the nano master).
 *
 * Differences from modbus_serial.c (handoff 92):
 *   - a frame ends at the library's T3.5 timer, not at the length derived from the
 *     function code, so an answer is seen one scan after its last byte;
 *   - the library does not say WHY an answer is bad: wrong CRC, short frame, wrong
 *     unit all arrive as MB_EIO and are reported as MODBUS_MASTER_ERR_CRC (a short
 *     frame is a timeout with the nano master);
 *   - the answer timeout counts from the start of the request (the transmit time of
 *     the frame is added to it).
 *
 * Every master that is init'ed owns one RS-485 channel of mb_port.c and, once the
 * first request starts, one library instance (re-opened when the baud changes).
 */

#include "modbus_serial.h"

#if SPLC_MODBUS_LIB == SPLC_MODBUS_LIB_MB

#include <string.h>

#include "mbport.h"
#include "mbm.h"
#include "common/mbframe.h"
#include "common/mbutils.h"

#include "mb_port.h"
#include "sx_time.h"
#include "splc_log.h"

static const char *TAG = "MB_MASTER";

#define FC_READ_COILS      0x01U
#define FC_READ_DISCRETE   0x02U
#define FC_READ_HOLDING    0x03U
#define FC_READ_INPUT      0x04U
#define FC_WRITE_COIL      0x05U
#define FC_WRITE_REG       0x06U
#define FC_WRITE_COILS     0x0FU
#define FC_WRITE_MULTIPLE  0x10U

#define MB_STEP_MAX        6U      /* library states walked in one drive step */
#define MB_DEFAULT_BAUD    9600UL  /* port->baudrate == 0 (host never told us) */
#define MB_MAX_MASTERS     (SPLC_MBM_ENABLE ? (SPLC_MBM_PORT_COUNT + 1U + (SPLC_ZIGBEE_ENABLE ? 1U : 0U)) : 1U)

/* Every master that went through init: lets a second init of the same object release
 * its library instance and channel first, without trusting the bytes of a struct the
 * caller never initialised. */
static modbus_rtu_master_t *s_reg[MB_MAX_MASTERS];

static bool mb_registered(const modbus_rtu_master_t *m)
{
    for (uint32_t i = 0; i < MB_MAX_MASTERS; i++) {
        if (s_reg[i] == m) { return true; }
    }
    return false;
}

static void mb_register(modbus_rtu_master_t *m)
{
    if (mb_registered(m)) { return; }
    for (uint32_t i = 0; i < MB_MAX_MASTERS; i++) {
        if (s_reg[i] == NULL) { s_reg[i] = m; return; }
    }
    log_error(TAG, "more masters than channels (%u)", (unsigned)MB_MAX_MASTERS);
}

static void mb_release(modbus_rtu_master_t *m)
{
    if (m->mb_hdl != NULL) {
        (void)eMBMClose((xMBMHandle)m->mb_hdl);
        m->mb_hdl = NULL;
    }
    if (m->mb_ch >= 0) {
        mb_port_serial_detach(m->mb_ch);
        m->mb_ch = -1;
    }
}

/* --- helpers (same rules as modbus_serial.c) ------------------------------ */

/* 3.5 character times (11 bits each), rounded up, at least 1 ms; with MBM never
 * shorter than the configured inter-frame delay of the port. */
static uint32_t frame_gap_ms(const modbus_rtu_master_t *m)
{
    uint32_t baud = m->port.baudrate;
    uint32_t gap  = 3U;
    if (baud != 0U) {
        gap = (38500U + baud - 1U) / baud;
        gap = (gap < 1U) ? 1U : gap + 1U;
    }
#if SPLC_MBM_ENABLE
    if (m->min_gap_ms > gap) { gap = m->min_gap_ms; }
#endif
    return gap;
}

static void finish(modbus_rtu_master_t *m, modbus_master_state_t st, modbus_master_error_t err)
{
    m->state          = st;
    m->error          = err;
    m->mb_qstate      = (uint8_t)MBM_STATE_NONE;
    m->quiet_until_ms = sx_get_tick_ms() + frame_gap_ms(m);
}

/* Hands out a finished result exactly once and returns the master to IDLE. */
static modbus_master_state_t take_result(modbus_rtu_master_t *m)
{
    modbus_master_state_t r = m->state;
    m->state = MODBUS_MASTER_IDLE;
    return r;
}

static bool port_can_tx(const modbus_serial_port_t *p)
{
#if SPLC_MBM_ENABLE
    return p->write != NULL || (p->write_start != NULL && p->tx_done != NULL);
#else
    return p->write != NULL;
#endif
}

static bool can_start(const modbus_rtu_master_t *m, uint8_t unit)
{
    if (m->mb_ch < 0 || m->port.read == NULL || !port_can_tx(&m->port) ||
        m->state != MODBUS_MASTER_IDLE || unit < 1U || unit > 247U) {
        return false;
    }
    return (int32_t)(sx_get_tick_ms() - m->quiet_until_ms) >= 0;
}

static uint32_t eff_baud(const modbus_rtu_master_t *m)
{
    return (m->port.baudrate != 0U) ? m->port.baudrate : (uint32_t)MB_DEFAULT_BAUD;
}

/* (Re)opens the library instance when there is none or the baud changed (T3.5 follows it). */
static bool ensure_instance(modbus_rtu_master_t *m)
{
    const uint32_t baud = eff_baud(m);
    if (m->mb_hdl != NULL && m->mb_baud == baud && m->mb_open_framing == m->mb_framing) { return true; }
    if (m->mb_hdl != NULL) {
        (void)eMBMClose((xMBMHandle)m->mb_hdl);
        m->mb_hdl = NULL;
    }
    xMBMHandle h = NULL;
    const eMBSerialMode mode = (m->mb_framing == SPLC_MODBUS_FRAMING_ASCII) ? MB_ASCII : MB_RTU;
    const eMBErrorCode e = eMBMSerialInit(&h, mode, (UCHAR)m->mb_ch, baud, MB_PAR_NONE);
    if (e != MB_ENOERR) {
        log_error(TAG, "eMBMSerialInit failed: err=%d ch=%d baud=%lu", (int)e, m->mb_ch, (unsigned long)baud);
        return false;
    }
    m->mb_hdl  = h;
    m->mb_baud = baud;
    m->mb_open_framing = m->mb_framing;
    mb_port_serial_set_baud(m->mb_ch, baud);
    return true;
}

/* Answer timeout handed to the library = the caller's timeout + the time the request
 * frame needs on the wire (the library's timer starts before the frame goes out). */
static uint16_t lib_timeout_ms(const modbus_rtu_master_t *m, uint16_t frame_len)
{
    uint32_t tx = 20U;
    if (m->mb_baud != 0U) {
        /* ASCII: ':' + 2 characters per byte (address + PDU + LRC) + CR LF; frame_len is the
         * RTU length (address + PDU + CRC16), so the ASCII body is 2*(frame_len - 1) + 3. */
        const uint32_t chars = (m->mb_framing == SPLC_MODBUS_FRAMING_ASCII)
                                   ? (2U * ((uint32_t)frame_len - 1U) + 3U)
                                   : (uint32_t)frame_len;
        tx += (chars * 11000U) / m->mb_baud;
    }
    uint32_t t = m->mb_timeout_ms + tx;
    if (t > 65535U) { t = 65535U; }
    return (uint16_t)t;
}

/* --- result mapping ------------------------------------------------------- */

static void complete(modbus_rtu_master_t *m, eMBErrorCode e)
{
    if (e == MB_ENOERR) {
        if (m->func == FC_READ_COILS || m->func == FC_READ_DISCRETE) {
            for (uint16_t i = 0U; i < m->qty; i++) {            /* LSB first */
                m->regs[i] = (uint16_t)((m->mb_bits[i >> 3] >> (i & 7U)) & 1U);
            }
        }
        finish(m, MODBUS_MASTER_DONE_OK, MODBUS_MASTER_ERR_NONE);
        return;
    }
    modbus_master_error_t err;
    if (e == MB_ETIMEDOUT) {
        err = MODBUS_MASTER_ERR_TIMEOUT;
    } else if (e >= MB_EX_ILLEGAL_FUNCTION && e <= MB_EX_GATEWAY_TARGET_FAILED) {
        err = MODBUS_MASTER_ERR_EXCEPTION;
        m->exception_code = (uint8_t)eMBErrorcodeToException(e);
    } else if (e == MB_EIO) {
        err = MODBUS_MASTER_ERR_CRC;      /* bad CRC / length / unit: the library does not say which */
    } else {
        err = MODBUS_MASTER_ERR_FRAME;
    }
    finish(m, MODBUS_MASTER_DONE_ERR, err);
}

/* One call of the library's polled function that matches the request in flight. */
static void lib_step(modbus_rtu_master_t *m, eMBMQueryState *st, eMBErrorCode *err)
{
    xMBMHandle h = (xMBMHandle)m->mb_hdl;
    switch (m->func) {
    case FC_READ_COILS:
        vMBMReadCoilsPolled(h, m->unit, m->addr, m->qty, m->mb_bits, st, err);
        break;
    case FC_READ_DISCRETE:
        vMBMReadDiscreteInputsPolled(h, m->unit, m->addr, m->qty, m->mb_bits, st, err);
        break;
    case FC_READ_HOLDING:
        vMBMReadHoldingRegistersPolled(h, m->unit, m->addr, (UBYTE)m->qty, m->regs, st, err);
        break;
    case FC_READ_INPUT:
        vMBMReadInputRegistersPolled(h, m->unit, m->addr, (UBYTE)m->qty, m->regs, st, err);
        break;
    case FC_WRITE_COIL:
        vMBMWriteSingleCoilPolled(h, m->unit, m->addr, (m->regs[0] != 0U) ? TRUE : FALSE, st, err);
        break;
    case FC_WRITE_REG:
        vMBMWriteSingleRegisterPolled(h, m->unit, m->addr, m->regs[0], st, err);
        break;
    case FC_WRITE_COILS:
        vMBMWriteCoilsPolled(h, m->unit, m->addr, m->qty, m->mb_bits, st, err);
        break;
    case FC_WRITE_MULTIPLE:
        vMBMWriteMultipleRegistersPolled(h, m->unit, m->addr, (UBYTE)m->qty, m->regs, st, err);
        break;
    default:
        *st  = MBM_STATE_DONE;
        *err = MB_EINVAL;
        break;
    }
}

/* One drive step (see the header comment). Finishes the request when the library does. */
static void drive(modbus_rtu_master_t *m)
{
    mb_port_serial_pump(m->mb_ch);                         /* 1. bytes, then timers */
    eMBMQueryState st  = (eMBMQueryState)m->mb_qstate;
    eMBErrorCode   err = MB_ENOERR;
    for (unsigned i = 0U; i < MB_STEP_MAX; i++) {          /* 2. library */
        const eMBMQueryState before = st;
        lib_step(m, &st, &err);
        if (st == MBM_STATE_DONE || st == before) { break; }
    }
    m->mb_qstate = (uint8_t)st;
    mb_port_serial_flush(m->mb_ch);                        /* 3. the frame out */
    if (st == MBM_STATE_DONE) { complete(m, err); }
}

/* Common start: the request fields are set by the caller, this opens the instance,
 * clears stale bytes, arms the answer timeout and sends the first step. */
static bool start(modbus_rtu_master_t *m, uint16_t frame_len, uint32_t timeout_ms)
{
    if (!ensure_instance(m)) {
        return false;
    }
    m->mb_timeout_ms = timeout_ms;
    (void)eMBMSetSlaveTimeout((xMBMHandle)m->mb_hdl, lib_timeout_ms(m, frame_len));
    mb_port_serial_drain(m->mb_ch);                        /* stale bytes from before */
    m->mb_qstate    = (uint8_t)MBM_STATE_NONE;
    m->error        = MODBUS_MASTER_ERR_NONE;
    m->state        = MODBUS_MASTER_BUSY;
    m->deadline_ms  = sx_get_tick_ms() + timeout_ms;
    drive(m);                                              /* frame out now, like the nano master */
    return true;
}

/* --- public API ----------------------------------------------------------- */

bool modbus_rtu_master_init(modbus_rtu_master_t *m, const modbus_serial_port_t *port)
{
    if (m == NULL) { return false; }
    if (mb_registered(m)) { mb_release(m); }               /* re-init of the same object */
    memset(m, 0, sizeof(*m));
    m->state = MODBUS_MASTER_IDLE;
    m->mb_ch = -1;
    if (port == NULL || port->read == NULL || !port_can_tx(port)) {
        return false;          /* port stays zeroed: every request is refused */
    }
    m->port = *port;
    m->mb_ch = mb_port_serial_attach(&m->port, (m->port.baudrate != 0U) ? m->port.baudrate
                                                                         : (uint32_t)MB_DEFAULT_BAUD);
    if (m->mb_ch < 0) {
        memset(&m->port, 0, sizeof m->port);
        return false;
    }
    mb_register(m);
    if (m->port.set_tx != NULL) {
        m->port.set_tx(m->port.ctx, false);                /* listen by default */
    }
    return true;
}

void modbus_rtu_master_set_framing(modbus_rtu_master_t *m, uint8_t framing)
{
    if (m == NULL) { return; }
    if (framing != SPLC_MODBUS_FRAMING_RTU && framing != SPLC_MODBUS_FRAMING_ASCII) { return; }
#if SPLC_MBM_ASCII_PORTS == 0
    if (framing == SPLC_MODBUS_FRAMING_ASCII) {
        log_error(TAG, "ASCII master: no port is built for it (SPLC_MBM_PORTn_FRAMING)");
        return;
    }
#endif
    m->mb_framing = framing;   /* ensure_instance() reopens the library instance on the next request */
}

static bool read_common(modbus_rtu_master_t *m, uint8_t unit, uint8_t fc, uint16_t addr,
                        uint16_t qty, uint32_t timeout_ms)
{
    if (qty < 1U || qty > MODBUS_MASTER_MAX_REGS || !can_start(m, unit)) {
        return false;
    }
    m->unit = unit;
    m->func = fc;
    m->addr = addr;
    m->qty  = qty;
    return start(m, 8U, timeout_ms);
}

#if SPLC_MBM_ENABLE
bool modbus_rtu_master_read_fc(modbus_rtu_master_t *m, uint8_t unit, uint8_t fc, uint16_t addr,
                               uint16_t qty, uint32_t timeout_ms)
{
    if (fc != FC_READ_COILS && fc != FC_READ_DISCRETE && fc != FC_READ_HOLDING && fc != FC_READ_INPUT) {
        return false;
    }
    return read_common(m, unit, fc, addr, qty, timeout_ms);
}
#endif

bool modbus_rtu_master_read(modbus_rtu_master_t *m, uint8_t unit, uint16_t addr, uint16_t qty,
                            uint32_t timeout_ms)
{
    return read_common(m, unit, FC_READ_HOLDING, addr, qty, timeout_ms);
}

bool modbus_rtu_master_write(modbus_rtu_master_t *m, uint8_t unit, uint16_t addr,
                             const uint16_t *values, uint16_t qty, uint32_t timeout_ms)
{
    if (values == NULL || qty < 1U || qty > MODBUS_MASTER_MAX_WRITE_REGS || !can_start(m, unit)) {
        return false;
    }
    for (uint16_t i = 0U; i < qty; i++) { m->regs[i] = values[i]; }
    m->unit = unit;
    m->func = FC_WRITE_MULTIPLE;
    m->addr = addr;
    m->qty  = qty;
    return start(m, (uint16_t)(9U + 2U * qty), timeout_ms);
}

#if SPLC_MBW_ENABLE
bool modbus_rtu_master_write_fc(modbus_rtu_master_t *m, uint8_t unit, uint8_t fc, uint16_t addr,
                                const uint16_t *values, uint16_t qty, uint32_t timeout_ms)
{
    if (values == NULL || qty < 1U) { return false; }
    if (fc == FC_WRITE_MULTIPLE) {
        return modbus_rtu_master_write(m, unit, addr, values, qty, timeout_ms);
    }
    if (!can_start(m, unit)) { return false; }

    if (fc == FC_WRITE_COIL || fc == FC_WRITE_REG) {
        if (qty != 1U) { return false; }
        m->regs[0] = (fc == FC_WRITE_COIL) ? ((values[0] != 0U) ? 1U : 0U) : values[0];
        m->unit = unit;
        m->func = fc;
        m->addr = addr;
        m->qty  = 1U;
        return start(m, 8U, timeout_ms);
    }
    if (fc == FC_WRITE_COILS) {
        if (qty > (uint16_t)(8U * sizeof m->mb_bits)) { return false; }
        const uint16_t nbytes = (uint16_t)((qty + 7U) / 8U);
        memset(m->mb_bits, 0, sizeof m->mb_bits);
        for (uint16_t i = 0U; i < qty; i++) {
            if (values[i] != 0U) { m->mb_bits[i >> 3] |= (uint8_t)(1U << (i & 7U)); }   /* LSB first */
        }
        m->unit = unit;
        m->func = fc;
        m->addr = addr;
        m->qty  = qty;
        return start(m, (uint16_t)(9U + nbytes), timeout_ms);
    }
    return false;
}
#endif

modbus_master_state_t modbus_rtu_master_poll(modbus_rtu_master_t *m)
{
    if (m->state == MODBUS_MASTER_DONE_OK || m->state == MODBUS_MASTER_DONE_ERR) {
        return take_result(m);
    }
    if (m->state != MODBUS_MASTER_BUSY) {
        return m->state;
    }
    drive(m);
    if (m->state == MODBUS_MASTER_DONE_OK || m->state == MODBUS_MASTER_DONE_ERR) {
        return take_result(m);
    }
    return MODBUS_MASTER_BUSY;
}

const uint16_t *modbus_rtu_master_regs(const modbus_rtu_master_t *m)       { return m->regs; }
modbus_master_error_t modbus_rtu_master_error(const modbus_rtu_master_t *m) { return m->error; }
uint8_t modbus_rtu_master_exception(const modbus_rtu_master_t *m)           { return m->exception_code; }

#if SPLC_MBM_ENABLE
void modbus_rtu_master_set_min_gap_ms(modbus_rtu_master_t *m, uint16_t gap_ms)
{
    m->min_gap_ms = gap_ms;
}

bool modbus_rtu_master_is_busy(const modbus_rtu_master_t *m)
{
    return m->state == MODBUS_MASTER_BUSY || m->state == MODBUS_MASTER_SENDING;
}
#endif

#endif /* SPLC_MODBUS_LIB == SPLC_MODBUS_LIB_MB */