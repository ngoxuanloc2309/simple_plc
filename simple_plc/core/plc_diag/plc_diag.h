#ifndef PLC_DIAG_H
#define PLC_DIAG_H

/*
 * plc_diag.h - Layer 2 (PLC Core)
 *
 * Diagnostic Control Block types (Wire Profile V2, 0x0A20..0x0A24), per
 * docs/SimplePLC_Wire_Contract_V2_Draft.md sections 2-4 and
 * docs/SimplePLC_App_MCU_Structs_v2.0_Self_Describing_Profile.md
 * section 3.5. This file must not include anything from Layer 0/1
 * (platform or driver headers) -- Layer 2 is the porting boundary and
 * must build on a plain PC toolchain.
 *
 * Scope: constants and enum definitions only, same as plc_device.h /
 * plc_system_cmd.h. There is deliberately no plc_diag.c. The state
 * machine owns RAM state and decodes Modbus frames, which is Layer 3
 * work -- see plc_modbus_cfg.c.
 *
 * SPLC_DiagErrorCode is NOT SPLC_ErrorCode (plc_error.h). Both have an
 * "invalid command" member, but they are different registers with
 * different value sets: SPLC_ErrorCode backs SYSTEM_COMMAND_RESULT
 * (0x0A01-0x0A02) and CONFIG_ERROR_CODE (0x9001); SPLC_DiagErrorCode
 * backs DIAG_ERROR_CODE (0x0A24) only. Do not merge or reuse them.
 *
 * All numeric values below are frozen by the wire contract -- they are
 * pinned explicitly, never auto-numbered.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Register addresses / lease constants (Wire Contract section 2) ---- */

#define SPLC_ADDR_DIAG_BLOCK          0x0A20U /* Base of the 5-register block */
#define SPLC_LEN_DIAG_BLOCK           5U

#define SPLC_MIN_DIAG_LEASE_MS        1000U
#define SPLC_DEFAULT_DIAG_LEASE_MS    3000U
#define SPLC_MAX_DIAG_LEASE_MS        60000U  /* Fits in uint16 */
#define SPLC_TARGET_HEARTBEAT_MS      1000U   /* Recommended host period */

/* Offsets inside the block (register units from SPLC_ADDR_DIAG_BLOCK). */
#define SPLC_DIAG_REG_COMMAND         0U
#define SPLC_DIAG_REG_STATE           1U
#define SPLC_DIAG_REG_FLAGS           2U
#define SPLC_DIAG_REG_LEASE_MS        3U
#define SPLC_DIAG_REG_ERROR_CODE      4U

/* --- DIAG_COMMAND (0x0A20) ---------------------------------------------- */

typedef enum {
    SPLC_DIAG_CMD_NONE           = 0,
    SPLC_DIAG_CMD_ENTER_DIAG     = 1, /* Acquire manual diagnostic ownership */
    SPLC_DIAG_CMD_HEARTBEAT      = 2, /* Reset lease timer to default duration */
    SPLC_DIAG_CMD_EXIT_DIAG      = 3, /* Release ownership, resume Rule Engine */
    SPLC_DIAG_CMD_COMMIT_RETAIN  = 4, /* Flush RAM retain shadow to Flash */
    SPLC_DIAG_CMD_DISCARD_RETAIN = 5  /* Reload RAM retain shadow from Flash */
} SPLC_DiagCommand;

/* --- DIAG_STATE (0x0A21, RO) -------------------------------------------- */

typedef enum {
    SPLC_DIAG_STATE_NONE           = 0,
    SPLC_DIAG_STATE_ENGINE_RUNNING = 1, /* Rule Engine owns the Tag Store */
    SPLC_DIAG_STATE_DIAG_CONTROL   = 2, /* Host owns the Tag Store */
    SPLC_DIAG_STATE_TRANSITIONING  = 3, /* Scan-boundary sync in progress */
    SPLC_DIAG_STATE_FAULT          = 4  /* Subsystem fault: diag locked */
} SPLC_DiagState;

/* --- DIAG_FLAGS (0x0A22, RO bitmask; bits 2..15 reserved, must be 0) ---- */

typedef enum {
    SPLC_DIAG_FLAG_RETAIN_DIRTY = (1 << 0), /* 0x0001: RAM retain != Flash */
    SPLC_DIAG_FLAG_LEASE_ACTIVE = (1 << 1)  /* 0x0002: Lease counting down */
} SPLC_DiagFlags;

/* --- DIAG_ERROR_CODE (0x0A24, RO, latching -- Wire Contract 4.5) -------- */

typedef enum {
    SPLC_DIAG_ERR_NONE               = 0,
    SPLC_DIAG_ERR_DENIED_FAULT       = 1, /* Rejected: system in FAULT state */
    SPLC_DIAG_ERR_LEASE_EXPIRED      = 2, /* Lease ran out / late heartbeat */
    SPLC_DIAG_ERR_FLASH_CRC_MISMATCH = 3, /* Flash commit verify failed */
    SPLC_DIAG_ERR_INVALID_COMMAND    = 4, /* Unrecognized / invalid-here cmd */
    SPLC_DIAG_ERR_RETAIN_DIRTY       = 5  /* Exit rejected: unsaved retain */
} SPLC_DiagErrorCode;

#ifdef __cplusplus
}
#endif

#endif /* PLC_DIAG_H */