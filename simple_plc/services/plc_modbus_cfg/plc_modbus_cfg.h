#ifndef PLC_MODBUS_CFG_H
#define PLC_MODBUS_CFG_H

/*
 * plc_modbus_cfg.h - Layer 3 (PLC Application Services)
 *
 * Modbus config/monitor server -- the App<->MCU channel per
 * docs/SimplePLC_App_MCU_Structs_v1.9_Self_Describing_Profile.md
 * section 8 (MODBUS REGISTER MAP V1). Owns exactly one nanoMODBUS server
 * instance (nmbs_t), backed by whichever concrete transport the caller
 * hands in as a modbus_transport_t (port/modbus_transport/
 * modbus_transport.h, Layer 3.5).
 *
 * This file is transport-agnostic on purpose: it does not include
 * sx_usb_cdc.h, sx_uart.h, or any other Layer 1 driver header, and does
 * not know or care whether the App is connected over USB-CDC (Remote I/O
 * SKU, see port/modbus_usb/modbus_usb.c), RS485/UART, or Modbus TCP
 * (future Gateway variants). Board init (Layer 4) is the only place that
 * knows which physical transport a given firmware build uses; it builds
 * the appropriate modbus_transport_t (e.g. via
 * modbus_transport_usb_create(), port/modbus_usb/modbus_usb.h) and passes
 * it to plc_modbus_cfg_init() below. See modbus_transport.h's own header
 * comment for the full rationale and the problem this replaced (a
 * previous version of this file took a sx_usb_tiny_t* directly).
 *
 * Scope: read-only exposure of DEVICE_DESCRIPTOR, DEVICE_RESOURCE_INFO,
 * ACTIVE_RULE_TABLE, DEVICE_HEALTH, RUNTIME_TAG_VALUES, SYSTEM_COMMAND(_RESULT),
 * plus the Rule Transfer staging/commit protocol at 0x9000-0xA001 (section
 * 9). This file owns the RAM instances of SPLC_DeviceDescriptor /
 * SPLC_DeviceResourceInfo / SPLC_DeviceHealth -- Layer 2 (plc_device.h)
 * only defines their types, per that header's own comment ("That
 * ownership belongs one layer up").
 *
 * May include Layer 0/1 headers directly (sx_time.h for uptime) --
 * Layer 3 is not required to build/test hardware-free, unlike Layer 2.
 * See docs/architecture.md section 3 for the full include-direction
 * table. The one Layer 1 header this file does NOT include on purpose is
 * any concrete transport driver header (sx_usb_cdc.h, sx_uart.h) -- see
 * the transport-agnostic note above.
 */

#include <stdbool.h>
#include <stdint.h>

#include "plc_device.h"
#include "plc_system_cmd.h"
#include "plc_error.h"
#include "modbus_transport.h"
#include "plc_rule.h" /* SPLC_RuleRecord, for the wire-format helpers below */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Device identity/resource instances this file owns and populates.
 * Layer 4 (board/app init) sets device_class/device_variant/hw_version/
 * resource counts once at boot before modbus_config_service() is ever
 * called; this file only exposes them read-only over Modbus and keeps
 * g_device_health's runtime fields (uptime_s, scan_time_ms, ...) current.
 *
 * Deliberately NOT static -- Layer 4's board init needs to write into
 * these once at boot (e.g. g_device_descriptor.hw_version_major = ...).
 */
extern SPLC_DeviceDescriptor    g_device_descriptor;
extern SPLC_DeviceResourceInfo  g_device_resource_info;
extern SPLC_DeviceHealth        g_device_health;

/*
 * One-time setup: creates the nanoMODBUS server instance, answering to
 * transport->unit_id (RTU only), bound to the read/write/process
 * functions transport provides.
 *
 * transport->unit_id MUST be 1..247 on RTU. nanoMODBUS rejects 0 (the
 * broadcast address) with NMBS_ERROR_INVALID_ARGUMENT, and at runtime it
 * silently IGNORES any request whose unit_id byte differs from this
 * value -- the App must send exactly this ID even on a point-to-point
 * link such as USB-CDC. (An earlier version of this comment claimed the
 * unit_id was "accepted but not checked"; that was wrong.)
 *
 * If nmbs_server_create() fails, this function logs the error and leaves
 * the Modbus server DISABLED: modbus_config_service() then still pumps
 * the transport but never calls nmbs_server_poll() (polling an
 * uninitialized nmbs_t would call a NULL function pointer -> HardFault).
 *
 * transport: caller-owned modbus_transport_t, already built by the
 *      appropriate factory function (e.g. modbus_transport_usb_create(),
 *      port/modbus_usb/modbus_usb.h) from a driver instance that has
 *      already been initialized (e.g. sx_usb_tiny_init() already called)
 *      before this call. This function copies the struct's contents
 *      internally; it does not store the pointer itself. The underlying
 *      driver instance transport->ctx refers to must outlive every
 *      future modbus_config_service() call (same instance passed as
 *      nmbs_platform_conf.arg for the lifetime of the server).
 *
 * Called once at boot, from plc_engine_init() (Layer 4), AFTER
 * tag_table_load_from_flash() and rule_table_load_from_flash() (so
 * DEVICE_RESOURCE_INFO's counts and RULE_TABLE_INFO's rule_count already
 * reflect real data when the App's first read request arrives) -- see
 * docs/architecture.md section 4's plc_engine_init() ordering.
 */
void plc_modbus_cfg_init(const modbus_transport_t *transport);

/*
 * Services one iteration of the Modbus server: pumps the transport
 * (transport->process(), if provided -- e.g. tud_task() for USB-CDC) so
 * RX/TX and connection state stay current even if no Modbus frame is
 * pending, then polls nanoMODBUS non-blockingly (nmbs_server_poll(), with
 * a byte/read timeout of 0 -- see plc_modbus_cfg_init()) for at most one
 * request/response. Never blocks: if no complete request is available,
 * both calls return immediately. Safe, and intended, to call every scan
 * cycle even when the App is idle or not connected at all.
 *
 * Also advances g_device_health.uptime_s (via sx_get_tick_ms(), Layer 1)
 * and updates health_flags from whatever plc_engine_scan_once() (Layer 4)
 * has recorded about the last scan's timing -- see
 * plc_modbus_cfg_record_scan_time() below.
 *
 * Called once per scan cycle (every 10 ms) from plc_engine_scan_once(),
 * BEFORE retain_service() -- see docs/architecture.md section 4.2's
 * ordering ("modbus_config_service()" is this function's name in that
 * doc's pseudocode) and plc_retain.h's own comment confirming that
 * order.
 */
void modbus_config_service(void);

/*
 * Records one scan cycle's duration into g_device_health.scan_time_ms /
 * max_scan_time_ms (the latter only updated if scan_time_ms exceeds the
 * previous max). Called by plc_engine_scan_once() (Layer 4) once per
 * cycle, immediately after timing the input_scan()/rule_scan()/
 * output_scan() sequence -- this file does not do its own timing, since
 * it has no visibility into where one scan cycle begins/ends from inside
 * modbus_config_service() alone.
 */
void plc_modbus_cfg_record_scan_time(uint32_t scan_time_ms);

/*
 * --- Wire-format CRC helpers (section 8.4) ------------------------------
 *
 * Exposed here (no longer `static` in plc_modbus_cfg.c) so
 * services/plc_rule_flash/plc_rule_flash.c can reuse the exact same
 * serialization/CRC this file already uses for ACTIVE_RULE_CRC16 /
 * EXPECTED_CRC16, per docs/handoff.md section 1.3 -- there must be only
 * ONE implementation of "how a rule table is turned into wire bytes" and
 * "how those bytes are CRC'd", shared by the Modbus path and the Flash
 * path, not two hand-written copies that could drift apart. See
 * plc_modbus_cfg.c's own header comment above these functions'
 * definitions for the full rationale (why RAM struct bytes are NOT used,
 * why nmbs_crc_calc() is NOT used here). Kept declared in this Layer 3
 * header (not moved to Layer 2) because CRC-16/MODBUS itself and this
 * specific wire byte order are a Modbus-protocol detail, not part of the
 * portable Rule Engine core.
 */

/* One step of CRC-16/MODBUS: XOR in `byte`, then 8 bitwise passes. */
uint16_t crc16_modbus_update(uint16_t crc, uint8_t byte);

/* Serializes one SPLC_RuleRecord to its 32-byte wire image (16 registers,
 * high byte first per register, High Word then Low Word per 32-bit
 * field). out must point to at least 32 writable bytes. */
void rule_record_to_wire(const SPLC_RuleRecord *r, uint8_t out[32]);

/* CRC-16/MODBUS over `count` records' wire images (count x 32 bytes) --
 * the plain arithmetic value (NOT byte-swapped for RTU framing). */
uint16_t rule_table_wire_crc16(const SPLC_RuleRecord *table, uint16_t count);

#ifdef __cplusplus
}
#endif

#endif /* PLC_MODBUS_CFG_H */