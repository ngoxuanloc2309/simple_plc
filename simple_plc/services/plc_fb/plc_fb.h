#ifndef PLC_FB_H
#define PLC_FB_H

/*
 * plc_fb.h - Layer 3 (PLC Application Services)
 *
 * The Function Block register block (0x0B00..0x0B7F, Wire Profile V2.0,
 * Structs doc sections 3.6 and 3.7): 8 Timer records at 0x0B00 and 8
 * Counter records at 0x0B40, each 8 registers (16 bytes).
 *
 * Division of ownership (agreed with the App team):
 *   Host writes (config, at Deploy, FC16):
 *     Timer   : mode (+1), pt_ms (+2..+3)
 *     Counter : mode (+1), preset_value (+2..+3), retain_tag_index (+6)
 *   Firmware owns (runtime telemetry, Host-written values are IGNORED):
 *     Timer   : status_bits (+0), et_ms (+4..+5), reserved (+6..+7)
 *     Counter : status_bits (+0), current_value (+4..+5), reserved (+7)
 *
 * The App sends its FB config as WHOLE 8-register blocks (zeros in the
 * firmware-owned fields), so a write that covers those fields is accepted
 * and the fields are simply skipped.
 *
 * Scope of this module today:
 *   - Timer: config is stored and read back. The timing itself still runs
 *     in the Rule Engine (the App emits macro rules, "dual generation"),
 *     so status_bits and et_ms read as 0. The firmware cannot compute them
 *     without knowing which tag drives IN.
 *   - Counter: current_value IS the COUNTER[i] tag (Counter i <-> tag
 *     tag_counter_base_index() + i, one piece of data seen from two
 *     addresses). Q (status bit 3) is derived from it and preset_value.
 *     CU / CD / RESET (bits 0..2) read as 0 for the same reason as Timer.
 *   - Not here yet: Flash persistence of the config, counter retain.
 *
 * This file is transport-agnostic: it does not include nanomodbus.h.
 * plc_modbus_cfg.c maps plc_fb_write()'s result onto Modbus exceptions.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PLC_FB_ADDR_BASE        0x0B00U
#define PLC_FB_ADDR_TIMER_BASE  0x0B00U
#define PLC_FB_ADDR_COUNTER_BASE 0x0B40U
#define PLC_FB_BLOCK_COUNT      8U      /* Timers, and separately Counters */
#define PLC_FB_BLOCK_REGS       8U      /* registers per record */
#define PLC_FB_REG_COUNT        (2U * PLC_FB_BLOCK_COUNT * PLC_FB_BLOCK_REGS)   /* 128 */

#define PLC_FB_RETAIN_NONE      0xFFFFU /* counter retain_tag_index: not retained */

/* SPLC_TimerMode_t / SPLC_CounterMode_t. 0 = block unused. */
#define PLC_FB_MODE_NONE        0U
#define PLC_FB_TIMER_TON        1U
#define PLC_FB_TIMER_TOF        2U
#define PLC_FB_TIMER_TP         3U
#define PLC_FB_COUNTER_CTU      1U
#define PLC_FB_COUNTER_CTD      2U

/* Counter status_bits */
#define PLC_FB_CNT_STATUS_Q     0x0008U

/*
 * Reset every block to "unused" (mode 0, preset 0, retain NONE). Call once
 * at boot, after tag_table_load_from_flash().
 */
void plc_fb_init(void);

/*
 * FC03 read of `quantity` registers starting at `offset` (0..127) inside
 * the block. Always succeeds; offset + quantity must stay inside the block
 * (the caller clamps each pass to the block's end address).
 */
void plc_fb_read(uint16_t offset, uint16_t quantity, uint16_t *registers_out);

typedef enum {
    PLC_FB_WRITE_OK = 0,
    PLC_FB_WRITE_BAD_ADDRESS,   /* -> Modbus 0x02 */
    PLC_FB_WRITE_BAD_VALUE      /* -> Modbus 0x03 */
} plc_fb_write_result_t;

/*
 * FC16 write (FC06 arrives as quantity 1). Any start offset and quantity
 * inside the block is accepted, so one request may carry a single field or
 * several whole records. Firmware-owned fields inside the span are skipped.
 *
 * All-or-nothing: the config of every record the request touches is
 * validated first; one invalid field leaves ALL records unchanged and
 * returns BAD_VALUE.
 *   - mode must be 0 (unused) or a valid mode for that block kind.
 *   - A Counter record whose COUNTER[i] tag does not exist on this board
 *     (the board has fewer counters) must have mode 0.
 *   - retain_tag_index must be PLC_FB_RETAIN_NONE or the index of a
 *     TAG_VREG_RETAIN tag, and no two counters may share one.
 * A request that runs past the end of the block returns BAD_ADDRESS.
 */
plc_fb_write_result_t plc_fb_write(uint16_t offset, uint16_t quantity,
                                   const uint16_t *registers);

#ifdef __cplusplus
}
#endif

#endif /* PLC_FB_H */