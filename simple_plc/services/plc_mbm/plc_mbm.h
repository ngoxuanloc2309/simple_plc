#ifndef PLC_MBM_H
#define PLC_MBM_H

/*
 * plc_mbm.h - Layer 3 (PLC Application Services)
 *
 * MB_READ: the device as a Modbus RTU MASTER that polls sensors on an RS485 bus
 * and copies the answers into tags (ICD SPLC-SPEC-MBM-001, docs/firmware/;
 * decisions 38..41 in docs/handoff.md). Up to 4 read blocks, each one a
 * poll transaction (slave, FC01/02/03/04, start address, count, interval,
 * timeout) with up to 4 pins that decode registers into tags.
 *
 * Wire (FC03 / FC06 / FC16, 4 blocks x 64 registers = 256 registers at 0x1000):
 *   +0 flags (bit 0 = enabled)      +1 (FC << 8) | slave        +2 start address
 *   +3 reg count                    +4..+5 poll_interval_ms     +6..+7 timeout_ms
 *   +8 valid tag   +9 error tag   +10 err_code tag   (0xFFFF = none)
 *   +11 pin count (0..4)   +12 read mode (0 cyclic, 1 on rising edge of a tag)
 *   +13 trigger tag (0xFFFF = none)   +14..+15 reserved
 *   +16 + 12*i  pin i: tag, reg_offset, data_type (1 BOOL 2 INT16 3 UINT16
 *               4 INT32 5 UINT32 6 REAL), endianness (0 ABCD 1 CDAB 2 BADC
 *               3 DCBA), multiplier (float32, High Word first), label (12 B)
 * The firmware decodes REGISTER by REGISTER (big-endian words, High byte first),
 * it never overlays a struct on the wire bytes. The source of truth for the
 * layout is the App codec ModbusMasterCodec.cs + simpleplc_protocol_v2_0.h.
 *
 * Staging (same idea as plc_mqtt / plc_tagtype): Host writes do NOT touch the
 * running blocks. They go to a DRAFT block (copied from the running block on
 * the first write to it, so a partial write keeps the rest). At COMMIT:
 *     plc_mbm_validate_draft() : false = reject the COMMIT, nothing changed
 *     plc_mbm_commit_draft()   : the drafted blocks become the running blocks
 *     plc_mbm_discard_draft()  : drop the draft
 * A Deploy that does not write a block keeps it running (the App writes 0 into
 * a block it no longer uses, which disables it). The 4 blocks are saved in the
 * Flash Rule Table record (flag bit 12, decision 41).
 *
 * Writing the tags (decision 40(b), App contract 2.9.0):
 *   - REAL pin   : value = float32 * multiplier, stored as float bits in the tag
 *                  (a NaN/Inf reading is dropped, the tag keeps its value).
 *   - integer pin: multiplier 1 -> the raw integer. Otherwise it looks the TARGET
 *                  TAG up in the tag-type table (plc_tagtype): REAL -> float bits
 *                  of raw * multiplier, anything else -> round(raw * multiplier).
 *   - BOOL pin   : 0 / 1.
 *   Without SPLC_REAL_ENABLE a tag cannot hold a float: REAL pins are rounded.
 *   A UINT32 above INT32_MAX wraps into the int32 tag slot.
 * Status tags (all optional): valid = 1 for ONE scan after a good answer;
 * error = 1 while the last poll failed; err_code = 0 none, 1 timeout (also a
 * failed transmit), 2 CRC, 3 slave exception, 4 malformed frame.
 *
 * Target tags must be written by the device and not by hardware: VFLAG, VREG,
 * VREG_RETAIN, MB_COIL or MB_HOLDING (a DI/DO/AI/COUNTER target is refused at
 * COMMIT). This is the firmware's reading of the ICD; the ICD is silent on it.
 *
 * Serial line settings (baud / parity / stop bits) are NOT in the wire yet:
 * only the App knows what the sensors speak, so they must come from the App.
 * Until the App team defines where, they come from splcopts.h
 * (SPLC_MBM_SERIAL_*) and plc_mbm_set_serial() is the one place a future wire
 * field plugs into. The host applies them with the configure() callback given
 * to splc_mbm_set_rs485_port().
 *
 * Scheduling: ONE request at a time, never blocking longer than the TX of one
 * short frame; round-robin over the due blocks. Not run while a diag session
 * suspends the engine (the tags are forced by the Host then).
 *
 * Threading: single-threaded, engine context only. Compiled only with
 * SPLC_MBM_ENABLE = 1, so a build with it 0 keeps its exact baseline size.
 */

#include <stdbool.h>
#include <stdint.h>

#include "splc_opt.h"

#if SPLC_MBM_ENABLE

#include "modbus_serial.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PLC_MBM_BASE          0x1000U
#define PLC_MBM_BLOCKS        4U
#define PLC_MBM_BLOCK_REGS    64U
#define PLC_MBM_REGS          (PLC_MBM_BLOCKS * PLC_MBM_BLOCK_REGS)   /* 256 */
#define PLC_MBM_FLASH_SIZE    (PLC_MBM_REGS * 2U)                     /* 512 */
#define PLC_MBM_MAX_PINS      4U

/* Capability bit MODBUS_MASTER of 0x0A08 (App header SPLC_CAP_MODBUS_MASTER). */
#define PLC_MBM_CAP_MODBUS_MASTER  0x00000100UL

typedef enum {
    PLC_MBM_WRITE_OK = 0,
    PLC_MBM_WRITE_BAD_ADDRESS      /* -> Modbus 0x02 */
} plc_mbm_write_result_t;

/* Line settings of the sensor bus. parity: 0 none, 1 odd, 2 even; stop_bits: 1 or 2. */
typedef struct {
    uint32_t baud;
    uint8_t  parity;
    uint8_t  stop_bits;
} plc_mbm_serial_t;

/* Host callback that re-configures its UART for the given line settings.
 * Returns 0 when the port now runs with them, negative when it cannot (the
 * firmware logs it and tries again about once a second). */
typedef int (*plc_mbm_configure_fn)(void *ctx, const plc_mbm_serial_t *cfg);

/* ---- host side -------------------------------------------------------- */

/* Register the RS485 port MB_READ talks through (copied; port->ctx is
 * borrowed). `configure` may be NULL when the host cannot change its UART: the
 * baudrate field of `port` is then taken as the real line speed and the
 * serial settings below are only informative. Call before plc_engine_init()
 * or any time before the first scan; not from another task. Without a port,
 * every block stays idle. */
void splc_mbm_set_rs485_port(const modbus_serial_port_t *port, plc_mbm_configure_fn configure);

/* ---- engine side ------------------------------------------------------ */

/* Boot: every block disabled, draft dropped, serial settings = defaults from
 * splcopts.h. Call once, before plc_rule_flash_load(). */
void plc_mbm_init(void);

/* Line settings. set_serial() returns false (nothing changed) for baud outside
 * 1200..921600, parity > 2 or stop_bits not 1/2; otherwise they are applied
 * through configure() on a later scan. The wire has no field for them yet
 * (see above), so nothing calls this except a future wire decoder. */
bool plc_mbm_set_serial(const plc_mbm_serial_t *cfg);
void plc_mbm_get_serial(plc_mbm_serial_t *out);

/* FC03 of `quantity` registers at `offset` (0..255): the RUNNING blocks. */
void plc_mbm_read(uint16_t offset, uint16_t quantity, uint16_t *registers_out);

/* FC16 (FC06 arrives as quantity 1) into the DRAFT. A request that runs past
 * the end of the area is BAD_ADDRESS and changes nothing. Content is checked
 * at COMMIT, not here. */
plc_mbm_write_result_t plc_mbm_write(uint16_t offset, uint16_t quantity, const uint16_t *registers);

bool plc_mbm_validate_draft(void);   /* true also when nothing was written */
void plc_mbm_commit_draft(void);     /* call only after validate returned true */
void plc_mbm_discard_draft(void);

/* Flash image: PLC_MBM_FLASH_SIZE bytes = the 256 RUNNING registers, big-endian. */
void plc_mbm_export(uint8_t *out);

/* Load an image written by plc_mbm_export(). The whole image is checked with
 * the COMMIT rules; if it fails NOTHING is applied and false is returned. Used
 * at boot and to roll back a failed CLEAR_RULES. Drops any draft. */
bool plc_mbm_import(const uint8_t *in);

/* CLEAR_RULES / FACTORY_RESET: every block disabled, draft dropped. */
void plc_mbm_clear_all(void);

/* One scan step (after output_scan): collect the answer of the request in
 * flight, then start at most one new request. Never blocks beyond one TX. */
void plc_mbm_scan(uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* SPLC_MBM_ENABLE */
#endif /* PLC_MBM_H */