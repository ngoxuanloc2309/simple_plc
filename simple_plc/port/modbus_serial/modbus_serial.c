#include "modbus_serial.h"

#include <string.h>

#include "splc_nmbs.h"   /* nmbs_crc_calc() only (renamed splc_nmbs_crc_calc) */
#include "sx_time.h"

#define FC_READ_COILS      0x01U
#define FC_READ_DISCRETE   0x02U
#define FC_READ_HOLDING    0x03U
#define FC_READ_INPUT      0x04U
#define FC_WRITE_MULTIPLE  0x10U

/* --- helpers ------------------------------------------------------------ */

/* CRC-16/MODBUS appended low byte first, as RTU framing wants it.
 * nmbs_crc_calc() returns the value byte-swapped for exactly this purpose
 * (see docs/handoff.md), so it is written big-endian here. */
static void put_crc(uint8_t *frame, uint16_t len_without_crc)
{
    uint16_t crc = nmbs_crc_calc(frame, len_without_crc, NULL);
    frame[len_without_crc]     = (uint8_t)(crc >> 8);
    frame[len_without_crc + 1] = (uint8_t)(crc & 0xFFU);
}

static bool crc_ok(const uint8_t *frame, uint16_t len)
{
    if (len < 4U) {
        return false;
    }
    uint16_t crc = nmbs_crc_calc(frame, (uint32_t)(len - 2U), NULL);
    return frame[len - 2U] == (uint8_t)(crc >> 8) &&
           frame[len - 1U] == (uint8_t)(crc & 0xFFU);
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFU);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

#if SPLC_MBM_ENABLE
static bool is_read_fc(uint8_t fc)
{
    return fc == FC_READ_COILS || fc == FC_READ_DISCRETE ||
           fc == FC_READ_HOLDING || fc == FC_READ_INPUT;
}

static bool is_bit_fc(uint8_t fc)
{
    return fc == FC_READ_COILS || fc == FC_READ_DISCRETE;
}

/* Bytes in a read answer's data section for this master's request. */
static uint16_t read_data_bytes(const modbus_rtu_master_t *m)
{
    return is_bit_fc(m->func) ? (uint16_t)((m->qty + 7U) / 8U)
                              : (uint16_t)(2U * m->qty);
}
#else
/* Remote I/O only: FC03, so the compiled code is exactly what it always was. */
#define is_read_fc(fc)       ((fc) == FC_READ_HOLDING)
#define is_bit_fc(fc)        (false)
#define read_data_bytes(m)   ((uint16_t)(2U * (m)->qty))
#endif

static void drain_rx(modbus_rtu_master_t *m)
{
    uint8_t scratch[16];
    /* Bounded: a babbling line must not trap the scan cycle here. */
    for (int i = 0; i < 16; i++) {
        if (m->port.read(m->port.ctx, scratch, (uint16_t)sizeof(scratch)) <= 0) {
            break;
        }
    }
}

/* 3.5 character times (11 bits each), rounded up, at least 1 ms; with MBM never
 * shorter than the configured inter-frame delay of the port. */
static uint32_t frame_gap_ms(const modbus_rtu_master_t *m)
{
    uint32_t baud = m->port.baudrate;
    uint32_t gap = 3U;
    if (baud != 0U) {
        gap = (38500U + baud - 1U) / baud;   /* 3.5 * 11 bits * 1000 / baud */
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
    m->quiet_until_ms = sx_get_tick_ms() + frame_gap_ms(m);
}

/* Hands out a finished result exactly once and returns the master to IDLE. */
static modbus_master_state_t take_result(modbus_rtu_master_t *m)
{
    modbus_master_state_t r = m->state;
    m->state = MODBUS_MASTER_IDLE;
    return r;
}

#if SPLC_MBM_ENABLE
/* True when the port can send at all: blocking write(), or the non-blocking pair. */
static bool port_can_tx(const modbus_serial_port_t *p)
{
    return p->write != NULL || (p->write_start != NULL && p->tx_done != NULL);
}

/* Non-blocking transmit: the frame is copied into m->rx (idle until the answer
 * starts), the transfer is started and the call returns at once. poll() watches
 * tx_done() and only then starts the answer timeout. */
static bool send_frame_async(modbus_rtu_master_t *m, const uint8_t *frame, uint16_t len,
                             uint32_t timeout_ms)
{
    drain_rx(m);                       /* stale bytes from before this request */
    memcpy(m->rx, frame, len);
    m->rx_len = 0U;

    if (m->port.set_tx != NULL) { m->port.set_tx(m->port.ctx, true); }
    if (m->port.write_start(m->port.ctx, m->rx, len) != 0) {
        if (m->port.set_tx != NULL) { m->port.set_tx(m->port.ctx, false); }
        finish(m, MODBUS_MASTER_DONE_ERR, MODBUS_MASTER_ERR_TX);
        return true;                   /* consumed; poll() reports the failure */
    }
    /* 11 bits per byte, doubled plus a margin: only a UART that never completes hits this. */
    uint32_t tx_ms = 50U;
    if (m->port.baudrate != 0U) {
        tx_ms = 2U * (((uint32_t)len * 11000U) / m->port.baudrate) + 50U;
    }
    m->tx_deadline_ms  = sx_get_tick_ms() + tx_ms;
    m->resp_timeout_ms = timeout_ms;
    m->error           = MODBUS_MASTER_ERR_NONE;
    m->state           = MODBUS_MASTER_SENDING;
    return true;
}
#endif

static bool send_frame(modbus_rtu_master_t *m, const uint8_t *frame, uint16_t len,
                       uint32_t timeout_ms)
{
#if SPLC_MBM_ENABLE
    if (m->port.write_start != NULL && m->port.tx_done != NULL) {
        return send_frame_async(m, frame, len, timeout_ms);
    }
#endif
    drain_rx(m);
    m->rx_len = 0U;

    if (m->port.set_tx != NULL) {
        m->port.set_tx(m->port.ctx, true);
    }
#if SPLC_MBM_ENABLE
    /* 11 bits per byte on the wire; never below the historic 50 ms. A 255-byte
     * frame at 9600 baud needs ~300 ms, so a fixed 50 ms would fail it. */
    uint32_t tx_ms = 50U;
    if (m->port.baudrate != 0U) {
        uint32_t need = ((uint32_t)len * 11000U) / m->port.baudrate + 20U;
        if (need > tx_ms) {
            tx_ms = need;
        }
    }
#else
    const uint32_t tx_ms = 50U;
#endif
    int rc = m->port.write(m->port.ctx, frame, len, tx_ms);
    if (m->port.set_tx != NULL) {
        m->port.set_tx(m->port.ctx, false);
    }
    drain_rx(m);   /* echo of our own frame, if the transceiver keeps RX open */

    if (rc != 0) {
        finish(m, MODBUS_MASTER_DONE_ERR, MODBUS_MASTER_ERR_TX);
        return true;   /* the request was consumed; poll() reports the failure */
    }
    m->deadline_ms = sx_get_tick_ms() + timeout_ms;
    m->state       = MODBUS_MASTER_BUSY;
    m->error       = MODBUS_MASTER_ERR_NONE;
    return true;
}

static bool can_start(const modbus_rtu_master_t *m, uint8_t unit)
{
    #if SPLC_MBM_ENABLE
    if (m->port.read == NULL || !port_can_tx(&m->port) ||
#else
    if (m->port.read == NULL || m->port.write == NULL ||
#endif
        m->state != MODBUS_MASTER_IDLE || unit < 1U || unit > 247U) {
        return false;
    }
    return (int32_t)(sx_get_tick_ms() - m->quiet_until_ms) >= 0;
}

/* --- public API --------------------------------------------------------- */

bool modbus_rtu_master_init(modbus_rtu_master_t *m, const modbus_serial_port_t *port)
{
    if (m == NULL) {
        return false;
    }
    memset(m, 0, sizeof(*m));
    m->state = MODBUS_MASTER_IDLE;
    #if SPLC_MBM_ENABLE
    if (port == NULL || port->read == NULL || !port_can_tx(port)) {
#else
    if (port == NULL || port->read == NULL || port->write == NULL) {
#endif
        return false;   /* port stays zeroed: every request is refused */
    }
    m->port = *port;
    if (m->port.set_tx != NULL) {
        m->port.set_tx(m->port.ctx, false);   /* listen by default */
    }
    return true;
}

#if SPLC_MBM_ENABLE
bool modbus_rtu_master_read_fc(modbus_rtu_master_t *m, uint8_t unit, uint8_t fc,
                               uint16_t addr, uint16_t qty, uint32_t timeout_ms)
{
    if (!is_read_fc(fc) || qty < 1U || qty > MODBUS_MASTER_MAX_REGS ||
        !can_start(m, unit)) {
        return false;
    }
    uint8_t f[8];
    f[0] = unit;
    f[1] = fc;
    put_u16(&f[2], addr);
    put_u16(&f[4], qty);
    put_crc(f, 6U);

    m->unit = unit; m->func = fc; m->addr = addr; m->qty = qty;
    return send_frame(m, f, 8U, timeout_ms);
}

bool modbus_rtu_master_read(modbus_rtu_master_t *m, uint8_t unit, uint16_t addr,
                            uint16_t qty, uint32_t timeout_ms)
{
    return modbus_rtu_master_read_fc(m, unit, FC_READ_HOLDING, addr, qty, timeout_ms);
}
#else
bool modbus_rtu_master_read(modbus_rtu_master_t *m, uint8_t unit, uint16_t addr,
                            uint16_t qty, uint32_t timeout_ms)
{
    if (qty < 1U || qty > MODBUS_MASTER_MAX_REGS || !can_start(m, unit)) {
        return false;
    }
    uint8_t f[8];
    f[0] = unit;
    f[1] = FC_READ_HOLDING;
    put_u16(&f[2], addr);
    put_u16(&f[4], qty);
    put_crc(f, 6U);

    m->unit = unit; m->func = FC_READ_HOLDING; m->addr = addr; m->qty = qty;
    return send_frame(m, f, 8U, timeout_ms);
}
#endif

bool modbus_rtu_master_write(modbus_rtu_master_t *m, uint8_t unit, uint16_t addr,
                             const uint16_t *values, uint16_t qty, uint32_t timeout_ms)
{
    if (values == NULL || qty < 1U || qty > MODBUS_MASTER_MAX_WRITE_REGS || !can_start(m, unit)) {
        return false;
    }
    uint8_t f[MODBUS_MASTER_FRAME_MAX];
    f[0] = unit;
    f[1] = FC_WRITE_MULTIPLE;
    put_u16(&f[2], addr);
    put_u16(&f[4], qty);
    f[6] = (uint8_t)(qty * 2U);
    for (uint16_t i = 0U; i < qty; i++) {
        put_u16(&f[7U + 2U * i], values[i]);
    }
    uint16_t len = (uint16_t)(7U + 2U * qty);
    put_crc(f, len);

    m->unit = unit; m->func = FC_WRITE_MULTIPLE; m->addr = addr; m->qty = qty;
    return send_frame(m, f, (uint16_t)(len + 2U), timeout_ms);
}

#if SPLC_MBW_ENABLE
bool modbus_rtu_master_write_fc(modbus_rtu_master_t *m, uint8_t unit, uint8_t fc,
                                uint16_t addr, const uint16_t *values, uint16_t qty,
                                uint32_t timeout_ms)
{
    if (values == NULL || qty < 1U) {
        return false;
    }
    if (fc == FC_WRITE_MULTIPLE) {
        return modbus_rtu_master_write(m, unit, addr, values, qty, timeout_ms);
    }
    if (!can_start(m, unit)) {
        return false;
    }
    uint8_t  f[MODBUS_MASTER_FRAME_MAX];
    uint16_t len;
    uint16_t echo = qty;                 /* what the slave must echo at bytes 4..5 */
    if (fc == 0x05U || fc == 0x06U) {
        if (qty != 1U) { return false; }
        const uint16_t v = (fc == 0x05U) ? ((values[0] != 0U) ? 0xFF00U : 0x0000U) : values[0];
        f[0] = unit; f[1] = fc;
        put_u16(&f[2], addr);
        put_u16(&f[4], v);
        len  = 6U;
        echo = v;                        /* FC05/FC06 echo address + value, not the quantity */
    } else if (fc == 0x0FU) {
        const uint16_t nbytes = (uint16_t)((qty + 7U) / 8U);
        if (qty > 1968U || (uint32_t)(9U + nbytes) > MODBUS_MASTER_FRAME_MAX) { return false; }
        f[0] = unit; f[1] = fc;
        put_u16(&f[2], addr);
        put_u16(&f[4], qty);
        f[6] = (uint8_t)nbytes;
        memset(&f[7], 0, nbytes);
        for (uint16_t i = 0U; i < qty; i++) {
            if (values[i] != 0U) { f[7U + (i >> 3)] |= (uint8_t)(1U << (i & 7U)); }   /* LSB first */
        }
        len = (uint16_t)(7U + nbytes);
    } else {
        return false;
    }
    put_crc(f, len);

    m->unit = unit; m->func = fc; m->addr = addr; m->qty = echo;
    return send_frame(m, f, (uint16_t)(len + 2U), timeout_ms);
}
#endif

/* Length of the complete answer once the first 2 bytes are known. */
static uint16_t expected_len(const modbus_rtu_master_t *m)
{
    if (m->rx[1] & 0x80U) {
        return 5U;                                   /* unit, func|0x80, code, crc */
    }
    if (is_read_fc(m->func)) {
        return (uint16_t)(5U + read_data_bytes(m));  /* unit, func, bytes, data, crc */
    }
    return 8U;                                       /* unit, func, addr, qty, crc */
}

modbus_master_state_t modbus_rtu_master_poll(modbus_rtu_master_t *m)
{
    if (m->state == MODBUS_MASTER_DONE_OK || m->state == MODBUS_MASTER_DONE_ERR) {
        return take_result(m);   /* e.g. a TX failure found at send time */
    }
    #if SPLC_MBM_ENABLE
    if (m->state == MODBUS_MASTER_SENDING) {
        if (m->port.tx_done(m->port.ctx)) {
            if (m->port.set_tx != NULL) { m->port.set_tx(m->port.ctx, false); }
            /* Answer timeout starts now. rx_len is 0 and the frame in rx[] is dead:
             * whatever the port delivers from here is the answer (may be queued already). */
            m->deadline_ms = sx_get_tick_ms() + m->resp_timeout_ms;
            m->state       = MODBUS_MASTER_BUSY;
        } else if ((int32_t)(sx_get_tick_ms() - m->tx_deadline_ms) >= 0) {
            if (m->port.write_abort != NULL) { m->port.write_abort(m->port.ctx); }
            if (m->port.set_tx != NULL) { m->port.set_tx(m->port.ctx, false); }
            finish(m, MODBUS_MASTER_DONE_ERR, MODBUS_MASTER_ERR_TX);
            return take_result(m);
        } else {
            return MODBUS_MASTER_BUSY;     /* still going out */
        }
    }
#endif
    if (m->state != MODBUS_MASTER_BUSY) {
        return m->state;
    }

    /* collect whatever arrived */
    int n = m->port.read(m->port.ctx, &m->rx[m->rx_len],
                         (uint16_t)(MODBUS_MASTER_FRAME_MAX - m->rx_len));
    if (n > 0) {
        m->rx_len = (uint16_t)(m->rx_len + (uint16_t)n);
    }

    if (m->rx_len >= 2U) {
        uint16_t want = expected_len(m);
        if (m->rx_len >= want) {
            bool ok = true;
            modbus_master_error_t err = MODBUS_MASTER_ERR_NONE;

            if (m->rx_len > want) {
                ok = false; err = MODBUS_MASTER_ERR_FRAME;      /* trailing garbage */
            } else if (!crc_ok(m->rx, want)) {
                ok = false; err = MODBUS_MASTER_ERR_CRC;
            } else if (m->rx[0] != m->unit) {
                ok = false; err = MODBUS_MASTER_ERR_FRAME;
            } else if (m->rx[1] & 0x80U) {
                ok = false; err = MODBUS_MASTER_ERR_EXCEPTION;
                m->exception_code = m->rx[2];
            } else if (m->rx[1] != m->func) {
                ok = false; err = MODBUS_MASTER_ERR_FRAME;
            } else if (is_read_fc(m->func)) {
                if (m->rx[2] != (uint8_t)read_data_bytes(m)) {
                    ok = false; err = MODBUS_MASTER_ERR_FRAME;
                } else if (is_bit_fc(m->func)) {
                    for (uint16_t i = 0U; i < m->qty; i++) {   /* LSB first */
                        m->regs[i] = (uint16_t)((m->rx[3U + (i >> 3)] >> (i & 7U)) & 1U);
                    }
                } else {
                    for (uint16_t i = 0U; i < m->qty; i++) {
                        m->regs[i] = get_u16(&m->rx[3U + 2U * i]);
                    }
                }
            } else {   /* FC16: the slave echoes address and quantity */
                if (get_u16(&m->rx[2]) != m->addr || get_u16(&m->rx[4]) != m->qty) {
                    ok = false; err = MODBUS_MASTER_ERR_FRAME;
                }
            }
            finish(m, ok ? MODBUS_MASTER_DONE_OK : MODBUS_MASTER_DONE_ERR, err);
            return take_result(m);
        }
    }

    if ((int32_t)(sx_get_tick_ms() - m->deadline_ms) >= 0) {
        finish(m, MODBUS_MASTER_DONE_ERR, MODBUS_MASTER_ERR_TIMEOUT);
        return take_result(m);
    }
    return MODBUS_MASTER_BUSY;
}

const uint16_t *modbus_rtu_master_regs(const modbus_rtu_master_t *m)
{
    return m->regs;
}

modbus_master_error_t modbus_rtu_master_error(const modbus_rtu_master_t *m)
{
    return m->error;
}

uint8_t modbus_rtu_master_exception(const modbus_rtu_master_t *m)
{
    return m->exception_code;
}

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