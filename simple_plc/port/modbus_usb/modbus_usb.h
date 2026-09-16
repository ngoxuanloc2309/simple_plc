#ifndef MODBUS_USB_H
#define MODBUS_USB_H

/*
 * port/modbus_usb/modbus_usb.h - Layer 3.5 (Protocol / Library Porting)
 *
 * Wraps components/usb_cdc/sx_usb_cdc.h (Layer 1) into the read()/write()
 * function-pointer shape libs/nanomodbus/nanomodbus.h expects in
 * nmbs_platform_conf, per docs/architecture.md section "Layer 3.5 -
 * Protocol / Library Porting (port/)", mục 2.4b.
 *
 * This is the App<->MCU channel: v1.9/v1.7 App loads Rules and reads
 * Device Health over Modbus RTU framing carried on USB-CDC, not RS485.
 * port/modbus_serial/ (kept separately, for the Gateway variant's Modbus
 * Master over real RS485) is a completely independent porting layer --
 * neither includes the other (docs/architecture.md section 3, "Bảng tổng
 * hợp - Ai được include ai").
 *
 * Per that same table, this header is included by exactly one file:
 * plc_modbus_cfg.c (Layer 3). Nothing else should include it directly.
 *
 * Ownership / arg convention: nanoMODBUS's nmbs_platform_conf carries a
 * single `void *arg` alongside the read/write function pointers, and
 * passes that same arg back into read()/write() on every call. Layer 1
 * here (sx_usb_cdc.c) never keeps a hidden global sx_usb_tiny_t instance
 * -- every sx_usb_tiny_*() call takes the instance pointer explicitly
 * from its caller (see sx_usb_tiny_process(), sx_usb_tiny_read(), etc.).
 * To stay consistent with that, modbus_usb_read()/modbus_usb_write()
 * below treat `arg` as a `sx_usb_tiny_t *`: the caller (plc_modbus_cfg.c)
 * owns exactly one sx_usb_tiny_t instance (same one it also drives with
 * sx_usb_tiny_process() each scan cycle) and passes it as
 * nmbs_platform_conf.arg when calling nmbs_client_create()/
 * nmbs_server_create() -- no separate wrapper struct, no global/static
 * instance hidden in this file.
 */

#include <stdint.h>
#include "sx_usb_cdc.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * nanoMODBUS platform read() function. Matches the exact signature
 * nmbs_platform_conf.read expects (libs/nanomodbus/nanomodbus.h).
 *
 * arg must be a sx_usb_tiny_t* (see "Ownership / arg convention" above)
 * that has already been initialized with sx_usb_tiny_init(). Blocks up
 * to timeout_ms waiting for count bytes to become available in the USB
 * CDC RX queue (draining it via sx_usb_tiny_read(), which itself pumps
 * tud_task() while waiting -- see sx_usb_cdc.c).
 *
 * Per nanoMODBUS's platform contract: a negative timeout_ms means
 * "block forever". sx_usb_tiny_read()'s timeout parameter is an
 * unsigned uint32_t millisecond count with no "forever" sentinel, so a
 * negative timeout_ms here is translated to UINT32_MAX rather than 0
 * (0 would mean "don't wait at all", the opposite of "forever").
 *
 * Returns the number of bytes actually read (0 <= n <= count) -- per
 * nanoMODBUS's contract, returning fewer than count bytes (including 0)
 * on timeout is correct and expected, not an error. Returns a negative
 * value only if sx_usb_tiny_read() itself signals a hard transport
 * error (see modbus_usb.c for what that currently means on top of
 * sx_usb_tiny_read()'s real return contract).
 */
int32_t modbus_usb_read(uint8_t *buf, uint16_t count, int32_t timeout_ms, void *arg);

/*
 * nanoMODBUS platform write() function. Matches the exact signature
 * nmbs_platform_conf.write expects (libs/nanomodbus/nanomodbus.h).
 *
 * arg must be a sx_usb_tiny_t* (see "Ownership / arg convention" above).
 * sx_usb_tiny_write() (Layer 1) has no timeout parameter of its own --
 * it already blocks internally, pumping tud_task() until every byte is
 * queued into the TinyUSB CDC TX FIFO, or the caller isn't connected (in
 * which case it returns without sending anything). timeout_ms is
 * accepted here only to satisfy nanoMODBUS's platform.write signature;
 * see modbus_usb.c for how the mismatch between "always blocks until
 * done" (sx_usb_tiny_write) and "respect timeout_ms, including 0 for
 * non-blocking" (nanoMODBUS's platform contract) is currently handled,
 * and what that means if byte_timeout_ms == 0 is ever actually needed.
 *
 * Returns count on success (sx_usb_tiny_write() has no partial-write
 * return value to report otherwise), or a negative value if not
 * connected -- see modbus_usb.c.
 */
int32_t modbus_usb_write(const uint8_t *buf, uint16_t count, int32_t timeout_ms, void *arg);

#ifdef __cplusplus
}
#endif

#endif /* MODBUS_USB_H */