#ifndef PLC_MBW_H
#define PLC_MBW_H

/*
 * plc_mbw.h - Layer 3 (PLC Application Services)
 *
 * MB_WRITE: the device as a Modbus RTU MASTER that writes tags to actuators on the
 * RS485 ports of plc_mbm (ICD SPLC-SPEC-MBM-002, docs/firmware/; decision 48 in
 * docs/handoff.md). Up to 4 write blocks, each in one of two modes:
 *
 *   Direct (write mode 0)   up to 4 pins (a tag -> register(s) of the slave) are
 *                           encoded (data type, endianness, multiplier) and written
 *                           with FC05 / FC06 / FC15 / FC16 to start_address.
 *   ChannelIndexed (mode 1) a channel tag, a command tag and a parameter tag drive
 *                           one of N identical channels of a slave (Sort-to-Light):
 *                             command register = base_cmd   + (channel - 1)          FC06
 *                             param registers  = base_param + (channel - 1) * 2      FC16 (High Word first)
 *                           With the Exclusive flag every OTHER lit channel is first
 *                           written command 0, then the parameter, then the command.
 *
 * Wire (FC03 / FC06 / FC16, 4 blocks x 64 registers = 256 registers at 0x1100), the
 * layout of ModbusMasterCodec.EncodeWriteBlock (App) / the ICD:
 *   +0 flags: bit 0 enabled, bit 1 exclusive channel, bits 2..3 write mode (0 Direct,
 *             1 ChannelIndexed), bits 4..7 RS-485 port index (0..3)
 *   +1 (FC << 8) | slave           +2 start address | base command address
 *   +3 point count | channel count +4..+5 base param address (uint32, High Word first)
 *   +6..+7 cyclic interval ms      +8..+9 timeout ms
 *   +10 trigger mode (0 rising edge of the trig tag, 1 on data change, 2 cyclic)
 *   +11 trig tag   +12 done tag   +13 busy tag   +14 error tag   +15 err_code tag (0xFFFF = none)
 *   +16.. Direct: 4 pins x 12 registers (tag, reg_offset, data_type, endianness,
 *         multiplier float32 High Word first, label) -- the layout of an MB_READ pin;
 *         a pin whose tag is 0xFFFF is unused.
 *         ChannelIndexed: +16 channel tag, +17 command tag, +18 parameter tag.
 * The firmware encodes REGISTER by REGISTER (big-endian words), it never overlays a
 * struct on the wire bytes.
 *
 * Staging, COMMIT, Flash (flag bit 9 of the Rule Table record, the last section),
 * CLEAR_RULES / FACTORY_RESET: exactly as plc_mbm / plc_mqtt_sub (host writes go to a
 * DRAFT, COMMIT validates the whole set first, a Deploy that does not write a block
 * keeps it running; a disabled block is not checked).
 *
 * Status tags (all optional, must be writable: VFLAG, VREG, VREG_RETAIN, MB_COIL,
 * MB_HOLDING): busy = 1 from the start of a job until it ends; done = 1 for ONE scan
 * after the whole job succeeded; error = 1 after a failed job (cleared when the next
 * job starts); err_code 0 none, 1 timeout (also a failed transmit or a port that is
 * not registered / disabled), 2 CRC or malformed answer, 3 slave exception, 4 the bus
 * stayed occupied for 2 s, 5 channel outside 1..channel_count, 6 (firmware extension)
 * a REAL source value that is NaN/Inf (nothing is sent).
 *
 * Triggers: rising edge of the trig tag (an edge during a job is remembered and starts
 * the next job), data change (any source tag differs from the value of the last job,
 * at most one job per interval_ms; the wire carries no deadband), cyclic (every
 * interval_ms). A Direct data-change block also writes once after COMMIT (baseline);
 * a ChannelIndexed one does not. The values are read from the tags when each frame is
 * built. A job is one frame in Direct mode and up to (lit channels + 2) frames in
 * Exclusive ChannelIndexed mode, one frame per transaction with the port released in
 * between. A failed frame is repeated up to the port's `retries` times (not after a
 * slave exception), then the job ends with the error.
 *
 * Bus: the ports belong to plc_mbm; each frame borrows one (plc_mbm_port_claim()), so
 * reads, writes and the App's line settings never overlap. A waiting write goes
 * before the reads of that port. plc_mbw_scan() runs BEFORE plc_mbm_scan().
 *
 * Threading: single-threaded, engine context only. Compiled only with
 * SPLC_MBW_ENABLE = 1 (which needs SPLC_MBM_ENABLE = 1), so a build with it 0 keeps its
 * exact baseline size.
 */

#include <stdbool.h>
#include <stdint.h>

#include "splc_opt.h"

#if SPLC_MBW_ENABLE

#ifdef __cplusplus
extern "C" {
#endif

#define PLC_MBW_BASE          0x1100U
#define PLC_MBW_BLOCKS        4U
#define PLC_MBW_BLOCK_REGS    64U
#define PLC_MBW_REGS          (PLC_MBW_BLOCKS * PLC_MBW_BLOCK_REGS)   /* 256 */
#define PLC_MBW_FLASH_SIZE    (PLC_MBW_REGS * 2U)                     /* 512 */
#define PLC_MBW_MAX_PINS      4U
#define PLC_MBW_MAX_COUNT     16U   /* Direct: registers / coils per frame the firmware accepts */
#define PLC_MBW_MAX_CHANNELS  32U   /* ChannelIndexed: channels per block (lit state is a 32-bit mask) */

typedef enum {
    PLC_MBW_WRITE_OK = 0,
    PLC_MBW_WRITE_BAD_ADDRESS      /* -> Modbus 0x02 */
} plc_mbw_write_result_t;

/* Boot: every block disabled, draft dropped. Call once, before plc_rule_flash_load(). */
void plc_mbw_init(void);

/* FC03 of `quantity` registers at `offset` (0..255): the RUNNING blocks. */
void plc_mbw_read(uint16_t offset, uint16_t quantity, uint16_t *registers_out);

/* FC16 (FC06 arrives as quantity 1) into the DRAFT. A request that runs past the end
 * of the area is BAD_ADDRESS and changes nothing. Content is checked at COMMIT. */
plc_mbw_write_result_t plc_mbw_write(uint16_t offset, uint16_t quantity, const uint16_t *registers);

bool plc_mbw_validate_draft(void);   /* true also when nothing was written */
void plc_mbw_commit_draft(void);     /* call only after validate returned true */
void plc_mbw_discard_draft(void);

/* Flash image: PLC_MBW_FLASH_SIZE bytes = the 256 RUNNING block registers, big-endian. */
void plc_mbw_export(uint8_t *out);

/* Load an image written by plc_mbw_export(). The whole image is checked with the COMMIT
 * rules; if it fails NOTHING is applied and false is returned. Drops any draft. */
bool plc_mbw_import(const uint8_t *in);

/* CLEAR_RULES / FACTORY_RESET: every block disabled, draft dropped. */
void plc_mbw_clear_all(void);

/* One scan step (BEFORE plc_mbm_scan): collect the answers, detect triggers, start at
 * most one frame per block. Never blocks beyond one TX. */
void plc_mbw_scan(uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* SPLC_MBW_ENABLE */
#endif /* PLC_MBW_H */