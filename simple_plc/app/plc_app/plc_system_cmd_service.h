#ifndef PLC_SYSTEM_CMD_SERVICE_H
#define PLC_SYSTEM_CMD_SERVICE_H

/*
 * plc_system_cmd_service.h - Layer 4 (Engine & Application entry)
 *
 * Executes the real-world effect of a SYSTEM_COMMAND (0x0A00) accepted by
 * services/plc_modbus_cfg/plc_modbus_cfg.c's write_system_command(). That
 * file only decodes the command and records it (Layer 3 is transport/
 * protocol-only, per its own header comment); calling into
 * sx_system_reset() (Layer 0/1) or Flash-erasing the rule/retain stores
 * belongs up here instead, per plc_system_cmd.h's (Layer 2) own comment
 * on where this execution boundary sits.
 *
 * Currently implements SPLC_SYSTEM_CMD_REBOOT end-to-end. See
 * plc_system_cmd_service.c's doc-comment for FACTORY_RESET/CLEAR_RULES/
 * CLEAR_RETAIN's status (accepted over Modbus, not yet acted on here --
 * open product-scope question, not a missing wiring step).
 */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Call once per scan cycle from plc_engine_scan_once(), AFTER
 * modbus_config_service() -- so that if this call ends up rebooting the
 * MCU, the FC06 ACK for the SYSTEM_COMMAND write that triggered it has
 * already been handed to the transport this same cycle, not merely
 * staged for "next time nmbs_server_poll() runs".
 *
 * Non-blocking in the common case (no command pending: one cheap
 * plc_modbus_cfg_get_pending_system_command() call, nothing else).
 * SPLC_SYSTEM_CMD_REBOOT specifically does NOT act within the same call
 * that first sees it -- see plc_system_cmd_service.c's doc-comment on
 * why it staggers the actual sx_system_reset() a few scan cycles later
 * (letting the ACK physically leave over USB first) rather than calling
 * sx_system_reset() immediately, which never returns.
 */
void plc_system_cmd_service(void);

#ifdef __cplusplus
}
#endif

#endif /* PLC_SYSTEM_CMD_SERVICE_H */