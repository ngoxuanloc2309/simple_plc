#ifndef MODBUS_LINK_H
#define MODBUS_LINK_H

/*
 * modbus_link.h - Layer 3.5 (Protocol / Library Porting)
 *
 * The App <-> device byte pipe, in the style of lwIP's netif. The core never
 * knows whether the bytes travel over USB-CDC, RS485 or a TCP socket; the
 * product (or a board file) plugs a pipe in with splc_link_register().
 *
 *     lwIP                     SimplePLC
 *     ---------------------    -----------------------------------
 *     netif_add()              splc_link_register()
 *     netif->linkoutput        splc_link_ops_t.output   (core -> peer)
 *     netif->input()           splc_link_input()        (peer -> core)
 *     sys_check_timeouts()     splc_link_ops_t.process  (optional pump)
 *
 * What the core owns (so that no port has to get it right):
 *   - the RX byte ring (SPLC_LINK_RX_BUF_SIZE),
 *   - every timeout: read() waits in real milliseconds, write() retries a
 *     partial output() until the link has made no progress for the timeout
 *     (an idle timeout: a long reply that a USB link takes piece by piece is
 *     not cut short by one small total budget). A frame longer than one USB packet
 *     (e.g. an FC16 of 73 bytes) arrives in several pieces; waiting for the
 *     next piece is done here, once, not in each port.
 *
 * What the port owns:
 *   - getting bytes to splc_link_input() when they arrive,
 *   - output(): hand bytes to the physical link.
 *
 * Threading contract (this is the whole of it):
 *   - splc_link_input() may be called from exactly ONE context (one task, or
 *     one ISR -- not a mix). It never blocks and is ISR-safe.
 *   - output(), process() and the transport functions returned by
 *     splc_link_get_transport() run in the engine context (the task that
 *     calls modbus_config_service()). If the physical driver is not safe to
 *     call from that task (e.g. TinyUSB owned by another task), output() must
 *     hand the bytes over to the driver's own task through a queue; it must
 *     not call the driver directly.
 *   - splc_link_register()/unregister() are init-time calls: no other call
 *     of this module may be running.
 *
 * Bytes passed to splc_link_input() before splc_link_register() are dropped.
 */

#include <stdint.h>
#include <stdbool.h>
#include "modbus_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SPLC_LINK_OK              =  0,
    SPLC_LINK_ERR_ARG         = -1,   /* NULL ops / output, bad kind or unit_id */
    SPLC_LINK_ERR_REGISTERED  = -2,   /* a link is already registered           */
    SPLC_LINK_ERR_NOT_READY   = -3    /* no link registered                     */
} splc_link_err_t;

typedef struct {
    /* Opaque pointer handed back unchanged to output() and process(). */
    void *ctx;

    /*
     * Core -> peer. Hand up to len bytes to the physical link without
     * blocking.
     *   return n, 0 <= n <= len : n bytes were accepted (0 = link busy, the
     *                             core retries until the link has made no
     *                             progress for its write timeout);
     *   return < 0              : hard error (e.g. USB not connected); the
     *                             core aborts the write.
     * A value greater than len is treated as a hard error.
     */
    int32_t (*output)(const uint8_t *buf, uint16_t len, void *ctx);

    /*
     * Optional. Called once per engine scan and while read()/write() wait.
     * For ports that have no task of their own (bare-metal): pump the driver
     * here (e.g. tud_task()) and call splc_link_input() with what arrived.
     * NULL when the port feeds splc_link_input() from its own context.
     */
    void (*process)(void *ctx);

    /* Modbus framing used on this link. */
    modbus_transport_kind_t kind;

    /* RTU slave address this device answers to, 1..247 (ignored for TCP). */
    uint8_t unit_id;
} splc_link_ops_t;

/* Register the one App link. The ops struct is copied. */
splc_link_err_t splc_link_register(const splc_link_ops_t *ops);

/* Remove the link and flush the RX ring (tests, or a controlled re-init). */
void splc_link_unregister(void);

/* True once splc_link_register() has succeeded. */
bool splc_link_is_registered(void);

/*
 * Peer -> core. Copy up to len received bytes into the RX ring and return how
 * many fit (< len when the ring is full; the rest are counted in
 * splc_link_rx_dropped() and the caller may retry later or discard them).
 */
uint16_t splc_link_input(const uint8_t *buf, uint16_t len);

/* Total bytes dropped because the RX ring was full, since registration. */
uint32_t splc_link_rx_dropped(void);

/*
 * Build the modbus_transport_t that plc_modbus_cfg_init() consumes.
 * Returns false (and leaves *out untouched) when no link is registered, so
 * the caller can report it instead of running a device that never answers.
 */
bool splc_link_get_transport(modbus_transport_t *out);

#ifdef __cplusplus
}
#endif

#endif /* MODBUS_LINK_H */