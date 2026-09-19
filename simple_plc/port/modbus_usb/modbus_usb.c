#include "modbus_usb.h"

#include <stdint.h>

/*
 * See modbus_usb.h for the full contract and the ownership / arg
 * convention (arg is always a sx_usb_tiny_t*, caller-owned, no global
 * instance kept in this file).
 */

int32_t modbus_usb_read(uint8_t *buf, uint16_t count, int32_t timeout_ms, void *arg)
{
    sx_usb_tiny_t *usb = (sx_usb_tiny_t *)arg;

    /*
     * nanoMODBUS's contract: timeout_ms < 0 means "block forever".
     * sx_usb_tiny_read()'s timeout parameter is uint32_t with no
     * "forever" sentinel of its own -- translate negative to the
     * largest value it accepts, rather than to 0 (0 there means "return
     * immediately", the opposite of "forever").
     *
     * A count of 0 needs no special case: sx_usb_tiny_read()'s own
     * while (len < _len && ...) loop condition is already false
     * immediately when _len == 0, so it returns 0 without blocking --
     * matching nanoMODBUS's expectation that read(buf, 0, ...) is a
     * trivial no-op success, not an error.
     */
    uint32_t usb_timeout_ms = (timeout_ms < 0) ? UINT32_MAX : (uint32_t)timeout_ms;

    int n = sx_usb_tiny_read(usb, buf, (uint32_t)count, usb_timeout_ms);

    /*
     * sx_usb_tiny_read() today (components/usb_cdc/sx_usb_cdc.c) never
     * itself returns a negative value -- its `len` accumulator only
     * ever counts upward from 0, so the worst case is 0 (nothing became
     * available before the timeout elapsed). That already matches
     * nanoMODBUS's "0..count-1 bytes back means a timeout occurred,
     * not an error" contract, so no extra negative-on-timeout mapping
     * is needed here today.
     *
     * This explicit (n < 0) guard stays in as defensive translation, in
     * case a future change to sx_usb_tiny_read() (e.g. detecting a USB
     * disconnect mid-read and signaling it with a negative return)
     * starts returning negative values -- nanoMODBUS requires that
     * anything other than 0..count be treated as a hard transport
     * error, and this keeps that translation centralized in one place
     * rather than relying on every caller to remember it.
     */
    if (n < 0) {
        return -1;
    }

    return (int32_t)n;
}

int32_t modbus_usb_write(const uint8_t *buf, uint16_t count, int32_t timeout_ms, void *arg)
{
    (void)timeout_ms; /* see modbus_usb.h -- sx_usb_tiny_write() has no
                        * timeout of its own; it always blocks internally
                        * until every byte is queued (or the port isn't
                        * connected). NOTE: this means a caller that asks
                        * for byte_timeout_ms == 0 ("write once, don't
                        * block, return immediately") on a connected but
                        * momentarily-stalled USB link will still block
                        * inside sx_usb_tiny_write()'s internal tud_task()
                        * pump loop, not return immediately as nanoMODBUS's
                        * platform contract technically allows for that
                        * case. Flagged here rather than silently
                        * "fixed" by guessing at non-blocking semantics
                        * sx_usb_tiny_write() doesn't actually have --
                        * needs a real decision (either give
                        * sx_usb_tiny_write() an actual timeout/
                        * non-blocking mode, or confirm the PLC's usage
                        * never calls nmbs_set_byte_timeout(nmbs, 0) for
                        * writes) before this is fully correct.
                        */

    sx_usb_tiny_t *usb = (sx_usb_tiny_t *)arg;

    if (!sx_usb_tiny_connected(usb)) {
        return -1;
    }

    /*
     * sx_usb_tiny_write() (components/usb_cdc/sx_usb_cdc.c) has no
     * partial-write return value -- its internal `while (sent < _len)`
     * loop only returns once every byte has been handed to
     * tud_cdc_write() (pumping tud_task() between calls if the TinyUSB
     * FIFO is momentarily full), or bails out early with nothing sent
     * at all if sx_usb_tiny_connected() was false at entry (already
     * checked above). There is therefore no way for this wrapper to
     * observe or report a genuine short write from the layer below it
     * today -- count is the only value that can honestly be returned on
     * the success path.
     */
    sx_usb_tiny_write(usb, buf, (uint32_t)count);

    return (int32_t)count;
}

/*
 * Adapter for modbus_transport_t.process: matches the void (*)(void *ctx)
 * shape modbus_transport.h expects, forwarding straight to
 * sx_usb_tiny_process() with the ctx pointer cast back to sx_usb_tiny_t*.
 * No logic of its own beyond the cast -- see modbus_transport_usb_create()
 * for why this indirection exists (modbus_transport_t cannot reference
 * sx_usb_tiny_t by name without including sx_usb_cdc.h from a
 * transport-agnostic header).
 */
static void modbus_transport_usb_process(void *ctx)
{
    sx_usb_tiny_process((sx_usb_tiny_t *)ctx);
}

modbus_transport_t modbus_transport_usb_create(sx_usb_tiny_t *usb, uint8_t unit_id)
{
    modbus_transport_t transport;

    transport.ctx      = usb;
    transport.read     = modbus_usb_read;
    transport.write    = modbus_usb_write;
    transport.process  = modbus_transport_usb_process;
    transport.kind     = MODBUS_TRANSPORT_KIND_RTU;
    transport.unit_id  = unit_id;

    return transport;
}