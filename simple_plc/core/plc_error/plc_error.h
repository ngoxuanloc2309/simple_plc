#ifndef PLC_ERROR_H
#define PLC_ERROR_H

/*
 * plc_error.h - Layer 2 (PLC Core)
 *
 * Common error code set, per the official data contract in
 * docs/SimplePLC_App_MCU_Structs_v1.7.md, section 7 (COMMON ERROR CODES).
 * This file must not include anything from Layer 0/1 (platform or driver
 * headers). This is the porting boundary: Layer 2 must build and unit test
 * on a plain PC toolchain, independent of any real hardware.
 *
 * Deliberately split out of plc_system_cmd.h into its own file. Per v1.7
 * section 7, SPLC_ErrorCode is explicitly a *shared* code set "so App and
 * MCU don't have to define separate errors per function" -- and the
 * register map backs that up: it is used independently in two unrelated
 * places, SYSTEM_COMMAND_RESULT (0x0A01-0x0A02, SPLC_SystemCommandResult)
 * and CONFIG_ERROR_CODE (0x9001, the rule transfer/commit state machine in
 * plc_modbus_cfg.c -- not yet written). Folding this into plc_system_cmd.h
 * would force plc_modbus_cfg.c to include SPLC_SystemCommand /
 * SPLC_CommandStatus just to get an error enum it has nothing to do with.
 *
 * See docs/architecture.md, section 2, "Layer 2 - PLC Core", and section 3
 * ("Ai duoc include ai").
 */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Common error code for command/config operations. Shared by
 * SPLC_SystemCommandResult.error_code (plc_system_cmd.h) and
 * CONFIG_ERROR_CODE (plc_modbus_cfg.c, not yet written) -- do not create a
 * second, function-specific error enum; extend this one instead if a new
 * error case is needed.
 */
typedef enum {
    SPLC_ERROR_NONE                 = 0, /* No error */
    SPLC_ERROR_INVALID_COMMAND      = 1, /* Command id not valid */
    SPLC_ERROR_INVALID_PARAMETER    = 2, /* Parameter not valid */
    SPLC_ERROR_BUSY                 = 3, /* MCU busy with another operation */
    SPLC_ERROR_CRC_MISMATCH         = 4, /* Data CRC did not match */
    SPLC_ERROR_UNSUPPORTED          = 5, /* Feature not supported */
    SPLC_ERROR_FLASH                = 6  /* Flash read/write error */
} SPLC_ErrorCode;

#ifdef __cplusplus
}
#endif

#endif /* PLC_ERROR_H */