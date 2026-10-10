#include "modbus_serial.h"

#include <string.h>

#include "nanomodbus.h"   /* nmbs_crc_calc() only */
#include "sx_time.h"

#define FC_READ_HOLDING    0x03U
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

static void drain_rx(modbus_rtu_master_t *m)
{
    uint8_t scratch[16];
    while (sx_uart_read(m->uart, scratch, (int)sizeof(scratch)) > 0) {
        /* discard */
    }
}

/* 3.5 character times (11 bits each), rounded up, at least 1 ms. */
static uint32_t frame_gap_ms(const modbus_rtu_master_t *m)
{
    uint32_t baud = (m->uart->config != NULL) ? m->uart->config->baudrate : 0U;
    if (baud == 0U) {
        return 3U;
    }
    uint32_t gap = (38500U + baud - 1U) / baud;   /* 3.5 * 11 bits * 1000 / baud */
    return (gap < 1U) ? 1U : gap + 1U;
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

static bool send_frame(modbus_rtu_master_t *m, const uint8_t *frame, uint16_t len,
                       uint32_t timeout_ms)
{
    drain_rx(m);
    m->rx_len = 0U;

    if (m->de_pin != NULL) {
        sx_gpio_write(m->de_pin, SX_GPIO_HIGH);
    }
    int rc = sx_uart_write(m->uart, frame, (int)len, 50U);
    if (m->de_pin != NULL) {
        sx_gpio_write(m->de_pin, SX_GPIO_LOW);
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
    if (m->state != MODBUS_MASTER_IDLE || unit < 1U || unit > 247U) {
        return false;
    }
    return (int32_t)(sx_get_tick_ms() - m->quiet_until_ms) >= 0;
}

/* --- public API --------------------------------------------------------- */

void modbus_rtu_master_init(modbus_rtu_master_t *m, sx_uart_t *uart, sx_gpio_pin_t *de_pin)
{
    memset(m, 0, sizeof(*m));
    m->uart   = uart;
    m->de_pin = de_pin;
    m->state  = MODBUS_MASTER_IDLE;
    if (de_pin != NULL) {
        sx_gpio_write(de_pin, SX_GPIO_LOW);   /* listen by default */
    }
}

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

bool modbus_rtu_master_write(modbus_rtu_master_t *m, uint8_t unit, uint16_t addr,
                             const uint16_t *values, uint16_t qty, uint32_t timeout_ms)
{
    if (values == NULL || qty < 1U || qty > MODBUS_MASTER_MAX_REGS || !can_start(m, unit)) {
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

/* Length of the complete answer once the first 2 bytes are known. */
static uint16_t expected_len(const modbus_rtu_master_t *m)
{
    if (m->rx[1] & 0x80U) {
        return 5U;                                   /* unit, func|0x80, code, crc */
    }
    if (m->func == FC_READ_HOLDING) {
        return (uint16_t)(5U + 2U * m->qty);         /* unit, func, bytes, data, crc */
    }
    return 8U;                                       /* unit, func, addr, qty, crc */
}

modbus_master_state_t modbus_rtu_master_poll(modbus_rtu_master_t *m)
{
    if (m->state == MODBUS_MASTER_DONE_OK || m->state == MODBUS_MASTER_DONE_ERR) {
        return take_result(m);   /* e.g. a TX failure found at send time */
    }
    if (m->state != MODBUS_MASTER_BUSY) {
        return m->state;
    }

    /* collect whatever arrived */
    int n = sx_uart_read(m->uart, &m->rx[m->rx_len], (int)(MODBUS_MASTER_FRAME_MAX - m->rx_len));
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
            } else if (m->func == FC_READ_HOLDING) {
                if (m->rx[2] != (uint8_t)(2U * m->qty)) {
                    ok = false; err = MODBUS_MASTER_ERR_FRAME;
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