#include "modbus_link.h"

#include <stdatomic.h>
#include <string.h>

#include "splc_opt.h"
#include "splc_log.h"
#include "sx_os.h"
#include "sx_time.h"

/*
 * See modbus_link.h for the contract. Notes on the implementation only.
 *
 * RX ring: single producer (splc_link_input(), one context, may be an ISR),
 * single consumer (link_read(), engine context). head is written only by the
 * producer, tail only by the consumer; each side reads the other's index with
 * acquire and publishes its own with release, so a byte is never read before
 * it is fully written and a slot is never reused before it is fully read. No
 * lock, no critical section. The indices run freely over uint32_t and are
 * masked on access; this is exact because SPLC_LINK_RX_BUF_SIZE is a power of
 * two (checked in splc_opt.h) and therefore divides 2^32.
 */

#define TAG "link"

#define RX_SIZE   ((uint32_t)SPLC_LINK_RX_BUF_SIZE)
#define RX_MASK   (RX_SIZE - 1U)

static splc_link_ops_t   s_ops;
static atomic_bool       s_registered;

static uint8_t           s_rx_buf[SPLC_LINK_RX_BUF_SIZE];
static atomic_uint_least32_t s_rx_head;      /* producer-owned */
static atomic_uint_least32_t s_rx_tail;      /* consumer-owned */
static atomic_uint_least32_t s_rx_dropped;

/* --- RX ring ----------------------------------------------------------- */

static uint32_t rx_pop(uint8_t *dst, uint32_t max)
{
    uint32_t tail = atomic_load_explicit(&s_rx_tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&s_rx_head, memory_order_acquire);
    uint32_t used = head - tail;
    uint32_t n    = (used < max) ? used : max;

    for (uint32_t i = 0; i < n; i++) {
        dst[i] = s_rx_buf[(tail + i) & RX_MASK];
    }
    atomic_store_explicit(&s_rx_tail, tail + n, memory_order_release);
    return n;
}

uint16_t splc_link_input(const uint8_t *buf, uint16_t len)
{
    if (buf == NULL || len == 0U ||
        !atomic_load_explicit(&s_registered, memory_order_acquire)) {
        return 0U;
    }

    uint32_t head  = atomic_load_explicit(&s_rx_head, memory_order_relaxed);
    uint32_t tail  = atomic_load_explicit(&s_rx_tail, memory_order_acquire);
    uint32_t space = RX_SIZE - (head - tail);
    uint32_t n     = (len < space) ? len : space;

    for (uint32_t i = 0; i < n; i++) {
        s_rx_buf[(head + i) & RX_MASK] = buf[i];
    }
    atomic_store_explicit(&s_rx_head, head + n, memory_order_release);

    if (n < len) {
        atomic_fetch_add_explicit(&s_rx_dropped, (uint32_t)len - n,
                                  memory_order_relaxed);
    }
    return (uint16_t)n;
}

uint32_t splc_link_rx_dropped(void)
{
    return atomic_load_explicit(&s_rx_dropped, memory_order_relaxed);
}

/* --- Pump -------------------------------------------------------------- */

static void link_pump(void)
{
    if (s_ops.process != NULL) {
        s_ops.process(s_ops.ctx);
    }
}

static void link_process(void *ctx)
{
    (void)ctx;
    if (atomic_load_explicit(&s_registered, memory_order_acquire)) {
        link_pump();
    }
}

/* --- Transport functions (engine context) ------------------------------ */

/* nanoMODBUS platform.read contract: return the bytes read (0..count); fewer
 * than count means the timeout elapsed. timeout_ms == 0: only what is already
 * there. timeout_ms < 0: wait forever.
 *
 * The timeout is an IDLE timeout (like link_write): it restarts every time at
 * least one byte arrives, and only a link that delivers nothing for timeout_ms
 * ends the read. nanoMODBUS asks for the whole rest of a request in ONE read()
 * (e.g. 98 bytes of an FC16 with 48 registers) and passes its BYTE timeout
 * (5 ms in plc_modbus_cfg). As a total budget that is too short on a serial
 * line: 98 bytes take 8.5 ms at 115200 baud, so a request still arriving when
 * the scan polls it was cut off, nanoMODBUS dropped the part it had read, and
 * the tail was misparsed as a new request (no reply at all). With an idle
 * timeout the length of the frame no longer matters, only a gap inside it. */
static int32_t link_read(uint8_t *buf, uint16_t count, int32_t timeout_ms, void *ctx)
{
    (void)ctx;

    if (!atomic_load_explicit(&s_registered, memory_order_acquire)) {
        return -1;
    }

    uint32_t start = sx_get_tick_ms();
    uint32_t got   = 0U;

    while (got < count) {
        uint32_t n = rx_pop(buf + got, (uint32_t)count - got);
        got += n;
        if (got >= count) {
            break;
        }
        if (n > 0U) {
            start = sx_get_tick_ms();        /* progress: restart the idle clock */
        }
        if (timeout_ms >= 0 &&
            (uint32_t)(sx_get_tick_ms() - start) >= (uint32_t)timeout_ms) {
            break;
        }

        /* Nothing more yet: let the producer run (RTOS: sleep one tick),
         * and pump a task-less port so it can deliver the next piece. The
         * wait is measured in real milliseconds, not loop passes. */
        link_pump();
        sx_os_yield_wait();
    }
    return (int32_t)got;
}

/* nanoMODBUS platform.write contract: return count on success, fewer than
 * count on timeout, negative on a hard error.
 *
 * The timeout is an IDLE timeout: it restarts every time output() accepts at
 * least one byte, and only a link that takes nothing for timeout_ms ends the
 * write. nanoMODBUS passes its BYTE timeout (5 ms in plc_modbus_cfg) for the
 * whole reply, and a reply of up to 255 bytes leaves a 64-byte-FIFO USB link in
 * several pieces, each waiting for the host to collect the previous one. A total
 * budget of 5 ms would cut such a reply short; a stalled link still aborts after
 * one idle timeout. timeout_ms == 0 stays "accept what fits right now". */
static int32_t link_write(const uint8_t *buf, uint16_t count, int32_t timeout_ms, void *ctx)
{
    (void)ctx;

    if (!atomic_load_explicit(&s_registered, memory_order_acquire)) {
        return -1;
    }

    uint32_t start = sx_get_tick_ms();
    uint32_t sent  = 0U;

    while (sent < count) {
        uint32_t remaining = (uint32_t)count - sent;
        int32_t  n = s_ops.output(buf + sent, (uint16_t)remaining, s_ops.ctx);

        if (n < 0 || (uint32_t)n > remaining) {
            return -1;                       /* hard error or broken port */
        }
        sent += (uint32_t)n;
        if (sent >= count) {
            break;
        }
        if (n > 0) {
            start = sx_get_tick_ms();        /* progress: restart the idle clock */
        }

        /* Partial or zero accept: the link is busy. Retry until it has made no
         * progress for the whole timeout. */
        if (timeout_ms >= 0 &&
            (uint32_t)(sx_get_tick_ms() - start) >= (uint32_t)timeout_ms) {
            break;
        }
        link_pump();
        sx_os_yield_wait();
    }
    return (int32_t)sent;
}

/* --- Registration ------------------------------------------------------ */

splc_link_err_t splc_link_register(const splc_link_ops_t *ops)
{
    if (ops == NULL || ops->output == NULL) {
        log_error(TAG, "register: ops/output is NULL");
        return SPLC_LINK_ERR_ARG;
    }
    if (ops->kind != MODBUS_TRANSPORT_KIND_RTU &&
        ops->kind != MODBUS_TRANSPORT_KIND_TCP) {
        log_error(TAG, "register: bad transport kind %d", (int)ops->kind);
        return SPLC_LINK_ERR_ARG;
    }
    if (ops->kind == MODBUS_TRANSPORT_KIND_RTU &&
        (ops->unit_id < 1U || ops->unit_id > 247U)) {
        log_error(TAG, "register: RTU unit_id %u not in 1..247", (unsigned)ops->unit_id);
        return SPLC_LINK_ERR_ARG;
    }
    if (atomic_load_explicit(&s_registered, memory_order_acquire)) {
        log_error(TAG, "register: a link is already registered");
        return SPLC_LINK_ERR_REGISTERED;
    }

    s_ops = *ops;
    atomic_store_explicit(&s_rx_head,    0U, memory_order_relaxed);
    atomic_store_explicit(&s_rx_tail,    0U, memory_order_relaxed);
    atomic_store_explicit(&s_rx_dropped, 0U, memory_order_relaxed);
    atomic_store_explicit(&s_registered, true, memory_order_release);

    log_info(TAG, "link registered, kind=%d unit_id=%u rx_ring=%u",
             (int)ops->kind, (unsigned)ops->unit_id, (unsigned)RX_SIZE);
    return SPLC_LINK_OK;
}

void splc_link_unregister(void)
{
    atomic_store_explicit(&s_registered, false, memory_order_release);
    atomic_store_explicit(&s_rx_head, 0U, memory_order_relaxed);
    atomic_store_explicit(&s_rx_tail, 0U, memory_order_relaxed);
    memset(&s_ops, 0, sizeof(s_ops));
}

bool splc_link_is_registered(void)
{
    return atomic_load_explicit(&s_registered, memory_order_acquire);
}

bool splc_link_get_transport(modbus_transport_t *out)
{
    if (out == NULL || !atomic_load_explicit(&s_registered, memory_order_acquire)) {
        return false;
    }

    out->ctx     = NULL;              /* the link is a singleton */
    out->read    = link_read;
    out->write   = link_write;
    out->process = link_process;
    out->kind    = s_ops.kind;
    out->unit_id = s_ops.unit_id;
    return true;
}