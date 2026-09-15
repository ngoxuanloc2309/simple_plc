#ifndef PLC_SYSTEM_CMD_H
#define PLC_SYSTEM_CMD_H

/*
 * plc_system_cmd.h - Layer 2 (PLC Core)
 *
 * System maintenance commands, per the official data contract in
 * docs/SimplePLC_App_MCU_Structs_v1.7.md, section 6 (SYSTEM COMMANDS) and
 * the SYSTEM_COMMAND / SYSTEM_COMMAND_RESULT register block (section 8.1,
 * 0x0A00-0x0A02). This file must not include anything from Layer 0/1
 * (platform or driver headers). This is the porting boundary: Layer 2 must
 * build and unit test on a plain PC toolchain, independent of any real
 * hardware.
 *
 * Scope of this file, same as plc_device.h: struct/enum type definitions
 * only. There is deliberately no plc_system_cmd.c here. Actually acting on
 * a command (rebooting, wiping Flash, clearing the rule/retain tables)
 * necessarily calls into Layer 0/1 (sx_flash_*, NVIC_SystemReset, ...),
 * which Layer 2 is not allowed to do. That implementation belongs one
 * layer up (Layer 3/4) -- per docs/handoff.md section 2.1, "CHUA co
 * implementation Layer 3 tuong ung (plc_system_cmd.c?)" is still an open
 * TODO there, not resolved by this file. This header only gives that
 * future implementation, and plc_modbus_cfg.c (which will decode
 * SYSTEM_COMMAND off the wire), a shared, spec-accurate vocabulary to work
 * with.
 *
 * SPLC_ErrorCode lives in a separate file, plc_error.h -- see that file's
 * header comment for why (it is shared with plc_modbus_cfg.c's
 * CONFIG_ERROR_CODE, unrelated to system commands).
 *
 * See docs/architecture.md, section 2, "Layer 2 - PLC Core".
 */

#include <stdint.h>

#include "plc_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Maintenance command the App can send down to the MCU. Not persistent
 * config -- these are one-shot actions.
 */
typedef enum {
    SPLC_SYSTEM_CMD_NONE           = 0,
    SPLC_SYSTEM_CMD_REBOOT         = 1, /* Reboot MCU, keep config/rules */
    SPLC_SYSTEM_CMD_FACTORY_RESET  = 2, /* Restore defaults per product policy */
    SPLC_SYSTEM_CMD_CLEAR_RULES    = 3, /* Clear the Active Rule Table */
    SPLC_SYSTEM_CMD_CLEAR_RETAIN   = 4  /* Clear retained data */
} SPLC_SystemCommand;

/* Execution status of a System Command. */
typedef enum {
    SPLC_CMD_STATUS_IDLE     = 0,
    SPLC_CMD_STATUS_ACCEPTED = 1,
    SPLC_CMD_STATUS_BUSY     = 2,
    SPLC_CMD_STATUS_DONE     = 3,
    SPLC_CMD_STATUS_ERROR    = 4
} SPLC_CommandStatus;

/*
 * Minimal command request, written by the App at SYSTEM_COMMAND (0x0A00,
 * WO).
 *
 * Wire size: 2 bytes (1 register).
 */
typedef struct {
    uint16_t command; /* SPLC_SystemCommand */
} SPLC_SystemCommandRequest; /* 2 bytes */

/*
 * Command result, read back by the App at SYSTEM_COMMAND_RESULT
 * (0x0A01-0x0A02, RO).
 *
 * Wire size: 4 bytes (2 registers).
 */
typedef struct {
    uint16_t status;     /* SPLC_CommandStatus */
    uint16_t error_code; /* SPLC_ErrorCode; 0 = SPLC_ERROR_NONE */
} SPLC_SystemCommandResult; /* 4 bytes */

#ifdef __cplusplus
}
#endif

#endif /* PLC_SYSTEM_CMD_H */