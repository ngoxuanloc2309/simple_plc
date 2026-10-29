#ifndef PLC_MBS_H
#define PLC_MBS_H

/*
 * plc_mbs.h - Layer 3. Modbus RTU SLAVE serving (SPLC-SPEC-SLAVE-001).
 *
 * An RS-485 port that the App set to the Slave role (port table 0x1400, +14 = 1, +15 = Slave
 * ID; see plc_mbm.h) is listened to here: a SCADA master reads and writes the device's tags
 * through a FIXED map, independent of the rule table. The UART binding and line settings come
 * from plc_mbm (the host's splc_mbm_set_rs485_port()); this module only reads and writes
 * bytes of a port that plc_mbm_port_role() says is a Slave. A Slave port is never a Master, so
 * nobody else touches its bytes.
 *
 * Function codes: FC01 / FC05 / FC15 (coils), FC02 (discrete inputs), FC03 / FC06 / FC16
 * (holding registers). Anything else (FC04 included) -> exception 0x01. Unit ID 0 is a
 * broadcast: writes are executed, nothing is sent back.
 *
 * Map (all 32-bit values are two registers, HIGH word first, like the rest of the wire):
 *   Holding   0x0000        DI_WORD     RO  bit i = DI i (DI 0..15; a bit with no DI reads 0)
 *             0x0001        DO_WORD     RW  bit i = DO i (a bit with no DO is ignored on write)
 *             0x0002..0x0003 VFLAG_DWORD RW  bit i = VFLAG i (0..31)
 *             0x0010..0x0017 AI0..3      RO  float32, one per two registers (see below)
 *             0x0020..0x005F VREG0..31   RW  the tag's 32 bits (INT32, or float bits of a REAL)
 *             0x0060..0x009F VREG_R0..31 RW  retained VREG
 *             0x00A0..0x00AF COUNTER0..7 RW  the counter tag
 *             0x00F0..0x00FF SYS_DIAG    RO  health image (see below)
 *   Coils     0x0000..0x000F DO0..15     RW
 *             0x0064..0x0083 VFLAG0..31  RW
 *   Discrete  0x0000..0x000F DI0..15     RO
 * A group entry the board does not have (DI 5 on a 4-DI board, VREG 20 when the layout has 12)
 * reads 0 and ignores writes, as long as the address is inside the fixed map.
 * Exceptions: function code not served 0x01; address outside the map, a hole between groups, or a
 * request crossing two regions 0x02 (DI_WORD 0x0000, DO_WORD 0x0001 and VFLAG_DWORD
 * 0x0002..0x0003 are THREE regions: CHKL-SLAVE-04); bad quantity / byte count
 * 0x03; write that touches a read-only register 0x03; write while a diagnostic session holds
 * the tags (the rule engine is suspended) 0x06.
 * FC06 on a 32-bit entry changes only the addressed half (FC16 is the way to write all of it).
 *
 * Firmware choices the ICD does not settle (change here, in plc_mbs.c):
 *  - The tag index of every group follows the board's dense layout (tag_*_base_index()), NOT
 *    the fixed indices in the ICD sample code.
 *  - AI: the tag's raw value as float32, i.e. (float)value for an INT32 tag and the stored
 *    bits for a REAL tag; no scaling. The ICD says "normalised voltage" without a formula.
 *  - SYS_DIAG: g_device_health as 10 registers (uptime_s 2, reset_reason, health_flags,
 *    cpu_load_percent, ram_usage_percent, scan_time_ms 2, max_scan_time_ms 2), then zeros.
 *  - Writes are refused (0x06) while the rule engine is suspended by a diagnostic session.
 *
 * Framing: a frame is complete as soon as its length is known (8 bytes, or 9 + byte count
 * for FC15/FC16) and its CRC is right, so the answer leaves in the same scan. Bytes of other
 * devices' frames on a shared bus are skipped by sliding one byte at a time until a valid
 * frame for this unit shows up; a rest that stays incomplete for 3.5 character times
 * (at least one scan) is dropped. No turnaround delay is configurable on a Slave (+5/+6/+7 of
 * its port record are ignored).
 *
 * Threading: single-threaded, engine context only. Compiled only with
 * SPLC_MODBUS_SLAVE_ENABLE = 1.
 */

#include <stdbool.h>
#include <stdint.h>

#include "splc_opt.h"

#if SPLC_MODBUS_SLAVE_ENABLE

#ifdef __cplusplus
extern "C" {
#endif

/* Boot: forget every partial frame. Call once, after plc_mbm_init(). */
void plc_mbs_init(void);

/* One engine scan: for every Slave port that is ready, collect bytes, answer complete
 * requests (at most one per port per scan) and drop dead partial frames. `rule_engine_suspended`
 * = a diagnostic session holds the tags (writes are refused with 0x06 then). */
void plc_mbs_scan(uint32_t now_ms, bool rule_engine_suspended);

/* Counters for diagnostics (since boot): requests answered, exceptions sent, frames dropped. */
uint32_t plc_mbs_stat_ok(void);
uint32_t plc_mbs_stat_exception(void);
uint32_t plc_mbs_stat_dropped(void);

/* ---- exposed for the PC test of the map; the engine does not call these ---- */

/* Execute one request PDU (function code first, no unit ID, no CRC) of `req_len` bytes and write
 * the response PDU to `rsp` (capacity >= 256). Returns the response length (an exception is 2
 * bytes), or 0 for "send nothing" (never for a unicast request). */
uint16_t plc_mbs_execute(const uint8_t *req, uint16_t req_len, uint8_t *rsp, bool writes_allowed);

#ifdef __cplusplus
}
#endif

#endif /* SPLC_MODBUS_SLAVE_ENABLE */
#endif /* PLC_MBS_H */