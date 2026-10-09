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
 *     Timer   : mode (+1), pt_ms (+2..+3), rule_ref (+6, Step 8d)
 *     Counter : mode (+1), preset_value (+2..+3), cv_tag_index (+6)
 *   Firmware owns (runtime telemetry, Host-written values are IGNORED):
 *     Timer   : status_bits (+0), et_ms (+4..+5), reserved (+7)
 *     Counter : status_bits (+0), current_value (+4..+5), reserved (+7)
 *
 * The App sends its FB config as WHOLE 8-register blocks (zeros in the
 * firmware-owned fields), so a write that covers those fields is accepted
 * and the fields are simply skipped.
 *
 * Staging (Step 8b, mirrors the Rule Table's staging buffer):
 *   Host writes do NOT touch the running config. They go to a DRAFT, and
 *   each block the Host writes (any register of it, firmware-owned ones
 *   included) is flagged as "written". The running config -- what FC03
 *   reads return and what is saved to Flash -- only changes at COMMIT:
 *     - plc_fb_commit_draft(): flagged blocks take their draft content,
 *       every other block becomes DISABLED (the config belongs to the
 *       program being committed, so a block the Deploy did not write is
 *       not part of it).
 *     - plc_fb_discard_draft(): throw the draft away (failed COMMIT).
 *   The draft is also dropped by plc_fb_init() (boot) and
 *   plc_fb_clear_all() (CLEAR_RULES / FACTORY_RESET). A block's draft
 *   starts as a copy of its running config the first time it is written,
 *   so a single-register write keeps the block's other fields.
 *
 * Scope of this module today:
 *   - Timer (Step 8d): config is stored and read back. The timing itself
 *     still runs in the Rule Engine (the App emits macro rules, "dual
 *     generation"); the firmware only REPORTS it. The Host says which rule
 *     does the timing in +6 (rule_ref = rule index + 1, 0 = none, so no
 *     sentinel clashes with rule 0). plc_fb_scan(), called once per scan
 *     after rule_scan(), reads that rule's runtime state and its action
 *     tag (the Q tag, set by all three timing rules) and fills:
 *       TON: RUNNING = rule is dwelling, Q = Q tag, ET = elapsed dwell
 *            (PT once Q), IN = dwelling or Q
 *       TOF: RUNNING = dwelling while Q is 1, IN = Q && !RUNNING
 *       TP : RUNNING = dwelling, IN not derivable (0)
 *     rule_ref is NOT checked against the committed rule count at write
 *     time (rules are committed after the FB config is written); a ref that
 *     points past the running table simply reports 0.
 *   - Counter: the count (CV) lives in a tag the Host chooses (the App's
 *     "Storage Register (CV)": VFLAG, VREG, VREG_RETAIN or COUNTER) and the
 *     Host sends that tag's index in the +6 register ("cv_tag_index"; the
 *     wire/Structs doc still call it retain_tag_index). The App's macro
 *     rules (INC_COUNTER ...) change the tag; this module only READS it:
 *       current_value = value of the CV tag (0 when cv_tag_index = 0xFFFF)
 *       Q (status bit 3) = CTU: CV >= PV, CTD: CV <= 0 (0 without a CV tag)
 *     A Counter is NOT tied to the tag COUNTER[i]; the two are unrelated.
 *     CU / CD / RESET (bits 0..2) read as 0 for the same reason as Timer.
 *     Persistence needs nothing here: a CV tag of kind VREG_RETAIN is saved
 *     and restored by plc_retain.c like any retain tag, and CLEAR_RETAIN /
 *     FACTORY_RESET clear it. A VFLAG/VREG CV tag is lost at reboot.
 *   - Flash persistence: plc_fb_export()/plc_fb_import() give the 128-byte
 *     image plc_rule_flash.c stores inside the Rule Table record
 *     (plc_fb_import_v1() reads the legacy 112-byte one, without rule_ref).
 *     This module itself never touches Flash.
 *
 * This file is transport-agnostic: it does not include nanomodbus.h.
 * plc_modbus_cfg.c maps plc_fb_write()'s result onto Modbus exceptions.
 */

#include <stdbool.h>
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

#define PLC_FB_CV_TAG_NONE      0xFFFFU /* counter cv_tag_index: no CV tag known */

/*
 * Flash image of the running config (big-endian, host-owned fields only):
 *   8 x Timer   { mode u16, pt_ms u32, rule_ref u16 }           = 8 x 8
 *   8 x Counter { mode u16, preset i32, cv_tag_index u16 }      = 8 x 8
 * = 128 bytes. Cross-checked against SPLC_RULE_FLASH_FB_SIZE in
 * plc_rule_flash.c. The legacy image (before Step 8d) has no rule_ref:
 *   8 x Timer { mode u16, pt_ms u32 } + 8 x Counter = 112 bytes.
 */
#define PLC_FB_FLASH_SIZE       128U
#define PLC_FB_FLASH_SIZE_V1    112U

/* Timer rule_ref: rule index + 1; 0 = this Timer is not bound to a rule. */
#define PLC_FB_RULE_REF_NONE    0U

/* SPLC_TimerMode_t / SPLC_CounterMode_t. 0 = block unused. */
#define PLC_FB_MODE_NONE        0U
#define PLC_FB_TIMER_TON        1U
#define PLC_FB_TIMER_TOF        2U
#define PLC_FB_TIMER_TP         3U
#define PLC_FB_COUNTER_CTU      1U
#define PLC_FB_COUNTER_CTD      2U

/* Timer status_bits */
#define PLC_FB_TMR_STATUS_IN       0x0001U
#define PLC_FB_TMR_STATUS_Q        0x0002U
#define PLC_FB_TMR_STATUS_RESET    0x0004U   /* not derivable: always 0 */
#define PLC_FB_TMR_STATUS_RUNNING  0x0008U

/* Counter status_bits */
#define PLC_FB_CNT_STATUS_Q     0x0008U

/*
 * Reset every block to "unused" (mode 0, preset 0, CV tag NONE) and drop
 * any draft. Call once at boot, after tag_table_load_from_flash().
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
 * The write goes to the DRAFT (see "Staging" above); the running config is
 * unchanged until plc_fb_commit_draft().
 *
 * All-or-nothing: the draft config of every block flagged as written
 * (including the ones this request touches) is validated first; one
 * invalid field leaves the draft unchanged and returns BAD_VALUE.
 *   - mode must be 0 (unused) or a valid mode for that block kind.
 *   - Timer rule_ref must be 0..MAX_RULES (see "Timer" above).
 *   - cv_tag_index must be PLC_FB_CV_TAG_NONE or the index of an existing
 *     VFLAG / VREG / VREG_RETAIN / COUNTER tag, and no two counters may
 *     share one. The duplicate check compares only blocks present in the
 *     draft: blocks the Deploy does not write become DISABLED at COMMIT, so
 *     the running config of those blocks must not cause a rejection.
 *     (A Counter does not need a COUNTER[i] tag to exist.)
 * A request that runs past the end of the block returns BAD_ADDRESS.
 */
plc_fb_write_result_t plc_fb_write(uint16_t offset, uint16_t quantity,
                                   const uint16_t *registers);

/*
 * Promote the draft to the running config (call after the Rule Table
 * commit succeeded, before saving to Flash): blocks flagged as written take
 * their draft content, all other blocks become DISABLED. The draft and its
 * flags are cleared.
 */
void plc_fb_commit_draft(void);

/* Drop the draft and its flags; the running config is untouched. */
void plc_fb_discard_draft(void);

/* Every block of the running config -> DISABLED, draft dropped.
 * Used by CLEAR_RULES / FACTORY_RESET. */
void plc_fb_clear_all(void);

/* Serialize the RUNNING config into PLC_FB_FLASH_SIZE bytes (see above). */
void plc_fb_export(uint8_t out[PLC_FB_FLASH_SIZE]);

/*
 * Load a PLC_FB_FLASH_SIZE-byte image as the running config (boot, or a
 * rollback). Validated like a Host write with every block counted as
 * present. Returns false -- running config unchanged -- if invalid (e.g. a
 * CV tag that no longer exists because the tag layout changed). The
 * draft is dropped either way.
 */
bool plc_fb_import(const uint8_t in[PLC_FB_FLASH_SIZE]);

/* Same for the legacy 112-byte image: Timers come back with rule_ref = none. */
bool plc_fb_import_v1(const uint8_t in[PLC_FB_FLASH_SIZE_V1]);

/*
 * Refresh the Timer telemetry (status_bits / ET). Layer 4 calls this once
 * per scan, right after rule_scan(), with the same now_ms. Read-only with
 * respect to the rules and tags.
 */
void plc_fb_scan(uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* PLC_FB_H */