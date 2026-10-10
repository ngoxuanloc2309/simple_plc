#ifndef SPLC_FLASH_DEFINE_H
#define SPLC_FLASH_DEFINE_H

#include "sx_platform_config.h"

/*
 * splc_flash_define.h - Layer 0 (platforms/stm32/stm32h5/flash_define)
 *
 * Flash memory map of the persistent-storage regions, as a set of macros --
 * not a driver, not business logic. Defines WHERE each region lives; the
 * driver (platforms/stm32/stm32h5/flash/stm32h5_flash.c) knows HOW to erase/
 * program a sector and services/plc_retain, services/plc_rule_flash know WHAT
 * goes in each region.
 *
 * The regions are ALWAYS the last 5 sectors of the chip's Flash, counted
 * backwards from its end. The only product input is the chip's Flash size,
 * SPLC_FLASH_SIZE_KB (splcopts.h, default in config/splc_opt.h); every
 * address below is derived from it, so moving to another STM32H5 part needs
 * no edit in this file. Every stm32h5_*.h file already #includes
 * sx_platform_config.h (which includes splc_opt.h), and the file is reached
 * by lower and upper layers through the PUBLIC include path of
 * platforms/stm32/stm32h5/CMakeLists.txt.
 *
 * Hardware facts this file is built on (STM32H5, RM0481 / datasheets):
 *   - Sector size 8 KB on every STM32H5 part (FLASH_SECTOR_SIZE in CMSIS).
 *   - Two banks of equal size (SPLC_FLASH_SIZE_KB / 2 each), so the last
 *     sectors are always in bank 2 and the code running from bank 1 keeps
 *     executing while they are erased/programmed. ASSUMPTION to re-check
 *     against the datasheet of any part not yet used: that part is dual-bank
 *     with equal banks (the checks below only prove the 5 sectors fit in
 *     bank 2).
 *   - FLASH_BASE (0x08000000) comes from the CMSIS device header; the size
 *     macros of that header (FLASH_SIZE/FLASH_BANK_SIZE) read a register
 *     that Hard-Faults on STM32H5 and must NOT be used.
 *
 * Layout (offsets counted back from END = FLASH_BASE + total size; the
 * example column is a 256 KB part, STM32H523CC):
 *
 *   Sector (from the end) | Address        | 256 KB example | Region
 *   ----------------------+----------------+----------------+-----------------
 *   last                  | END - 1 sector | 0x0803E000     | Rule Table A (running copy)
 *   2nd from last         | END - 2        | 0x0803C000     | Rule Table B (backup copy)
 *   3rd from last         | END - 3        | 0x0803A000     | Retain sector 3 of 3
 *   4th from last         | END - 4        | 0x08038000     | Retain sector 2 of 3
 *   5th from last         | END - 5        | 0x08036000     | Retain sector 1 of 3 (lowest)
 *
 * The firmware image (.text/.data) must stay below SPLC_FLASH_DATA_BASE_ADDR
 * (END - 5 sectors): set the linker script's FLASH LENGTH to
 * SPLC_FLASH_SIZE_KB minus SPLC_FLASH_RESERVED_SIZE (40 KB).
 *
 * Rule Table: 2 sectors (A/B, 8 KB each), per docs/handoff.md section 1 --
 * see services/plc_rule_flash/plc_rule_flash.h for the full A/B recovery
 * mechanism this supports. Each sector holds one on-Flash record: an
 * 8-byte header (seq_num/rule_count/crc16) plus up to
 * MAX_RULES(100) * sizeof(SPLC_RuleRecord)(32) = 3200 bytes of rule wire
 * data (3208 bytes total max, 39% of one 8 KB sector) -- see
 * core/plc_rule/plc_rule.h. No wear-leveling within a sector (each holds
 * exactly one record, rewritten in place): the Active Rule Table only
 * changes when the App commits a new rule set (rare, human-triggered),
 * unlike Retain which can change every scan cycle -- see
 * docs/architecture.md section 2.6.2 for the commit protocol (staging
 * happens over Modbus registers in RAM/App side; only the final
 * committed table is written to Flash, to BOTH A and B).
 *
 * Retain: 3 contiguous sectors (24 KB) using the rotating EEPROM-emulation
 * scheme from docs/SimplePLC_RuleStruct_MCU_Spec_v0.1.md section 7,
 * updated for v1.9's 32-slot VREG_RETAIN range (the v0.1 spec's own
 * numbers -- 104 byte/record, ~78 records/sector, 64 KB/8 sectors -- were
 * computed for the OLD 16-slot VREG_RETAIN layout and do not apply
 * as-is; see the recomputed constants below). Reduced from the original
 * 4 sectors to 3 to free one sector (the former 4th Retain sector) for Rule Table B
 * -- per docs/handoff.md section 1.1 point 1, this trade was made
 * deliberately with the user rather than growing the total reserved
 * Flash region.
 *
 * Endurance at these 3 sectors, 32 retain slots, RETAIN_SNAPSHOT_PERIOD_MS
 * = 5 minutes (docs/SimplePLC_RuleStruct_MCU_Spec_v0.1.md section 7.1):
 * still comfortably sufficient (on the order of ten years) before
 * exhausting the ~10,000 erase-cycle endurance typically quoted for
 * STM32H5 Flash sectors, even after losing one sector to Rule Table B --
 * accepted as sufficient for this product's expected service life (see
 * chat discussion; no consumer gateway device is expected to still be the
 * same physical unit in active use after 10+ years). If
 * RETAIN_SNAPSHOT_PERIOD_MS is ever configured shorter over Modbus
 * (docs/architecture.md's config knob), this endurance budget shrinks
 * proportionally -- worth re-checking before allowing very short periods.
 */

#if STM32H5_PLATFORM

#include <stdint.h>

/* FLASH_BASE (0x08000000) is a CMSIS device-header macro (Drivers/CMSIS/
 * Device/ST/STM32H5xx/Include/stm32h523xx.h), needed below by
 * SPLC_FLASH_RULE_TABLE_ADDR/SPLC_FLASH_RETAIN_BASE_ADDR. This file is
 * consumed from Layer 3 (services/plc_retain.c) as well as Layer 0
 * (platforms/stm32/stm32h5/), and Layer 3 has no reason to already have
 * included any HAL/CMSIS header itself before reaching for "where is
 * Flash laid out" -- unlike a stm32h5_*.h file, which always sits behind
 * `#if STM32H5_PLATFORM` and can assume stm32h5xx_hal.h is already
 * pulled in by the same guard. So this header includes it directly,
 * making itself self-contained rather than relying on whoever included
 * it to have done so first. Safe to include unconditionally (not gated
 * behind STM32H5_PLATFORM) because this file, by construction, only
 * ever describes the STM32H5's own Flash layout -- a build for a
 * different chip would use a different splc_flash_define.h entirely,
 * not this same file with a different platform branch inside it. */
#include "stm32h5xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- Flash geometry (derived from SPLC_FLASH_SIZE_KB) ------------------ */
#define SPLC_FLASH_SECTOR_SIZE          0x2000U                                  /* 8 KB, all STM32H5 */
#define SPLC_FLASH_TOTAL_SIZE           ((uint32_t)SPLC_FLASH_SIZE_KB * 1024U)
#define SPLC_FLASH_BANK_SIZE            (SPLC_FLASH_TOTAL_SIZE / 2U)             /* two equal banks */
#define SPLC_FLASH_SECTOR_COUNT         (SPLC_FLASH_TOTAL_SIZE / SPLC_FLASH_SECTOR_SIZE)
#define SPLC_FLASH_END_ADDR             (FLASH_BASE + SPLC_FLASH_TOTAL_SIZE)     /* one past the last byte */

/* Sectors reserved for data at the end of Flash: Rule Table A/B + Retain. */
#define SPLC_FLASH_RULE_TABLE_SECTORS   2U
#define SPLC_FLASH_RETAIN_SECTOR_COUNT  3U
#define SPLC_FLASH_RESERVED_SECTORS     (SPLC_FLASH_RULE_TABLE_SECTORS + SPLC_FLASH_RETAIN_SECTOR_COUNT)
#define SPLC_FLASH_RESERVED_SIZE        (SPLC_FLASH_RESERVED_SECTORS * SPLC_FLASH_SECTOR_SIZE)   /* 40 KB */
/* Lowest address of the reserved region; the firmware must end below it. */
#define SPLC_FLASH_DATA_BASE_ADDR       (SPLC_FLASH_END_ADDR - SPLC_FLASH_RESERVED_SIZE)

/* A bad SPLC_FLASH_SIZE_KB must stop the build, not erase code at run time. */
#ifndef __cplusplus
_Static_assert((SPLC_FLASH_SIZE_KB) % 16 == 0,
               "SPLC_FLASH_SIZE_KB must be a multiple of 16 (two banks of whole 8 KB sectors)");
_Static_assert((SPLC_FLASH_SIZE_KB) >= 128 && (SPLC_FLASH_SIZE_KB) <= 2048,
               "SPLC_FLASH_SIZE_KB outside the 128..2048 KB range of the STM32H5 family");
_Static_assert(SPLC_FLASH_RESERVED_SIZE <= SPLC_FLASH_BANK_SIZE,
               "data sectors do not fit in bank 2 of this Flash size");
#endif

/* --- Rule Table A/B (see services/plc_rule_flash/plc_rule_flash.h) ------
 *
 * Last two sectors in Flash. A is the
 * "running" copy that plc_rule_flash_load() reads first at boot; B is its
 * backup, used to self-heal A if A's CRC is ever bad (e.g. power loss
 * mid-write) -- see plc_rule_flash.h for the full mechanism. Both sectors
 * use the identical on-Flash record format; app/architecture docs refer
 * to this pair collectively as "the Rule Table Flash region". See
 * docs/architecture.md section 2.6.2 for the Modbus staging/commit
 * protocol that produces what eventually gets written here.
 */
#define SPLC_FLASH_RULE_TABLE_A_ADDR    (SPLC_FLASH_END_ADDR - 1U * SPLC_FLASH_SECTOR_SIZE)
#define SPLC_FLASH_RULE_TABLE_B_ADDR    (SPLC_FLASH_END_ADDR - 2U * SPLC_FLASH_SECTOR_SIZE)
#define SPLC_FLASH_RULE_TABLE_SIZE      SPLC_FLASH_SECTOR_SIZE   /* 1 sector each, 8 KB */

/* --- Retain (VREG_RETAIN rotating EEPROM-emulation store) ---------------
 *
 * 3 contiguous sectors immediately before Rule Table B (the 5th..3rd
 * sectors from the end of Flash), per docs/SimplePLC_RuleStruct_MCU_Spec_v0.1.md section 7.1's
 * rotating-record scheme: services/plc_retain/plc_retain.c scans this
 * whole region at boot for the record with the highest seq_num and a
 * valid CRC to find the "active" write position, rather than persisting
 * a separate pointer anywhere. Reduced from 4 to 3 sectors to free a sector
 * for Rule Table B -- see the header comment above.
 *
 * SPLC_FLASH_RETAIN_BASE_ADDR is the LOWEST address of the 3-sector
 * region (5th sector from the end) -- i.e. writing/scanning proceeds
 * from SPLC_FLASH_RETAIN_BASE_ADDR upward through
 * SPLC_FLASH_RETAIN_BASE_ADDR + SPLC_FLASH_RETAIN_TOTAL_SIZE - 1, which
 * ends exactly where SPLC_FLASH_RULE_TABLE_B_ADDR begins (no gap, no
 * overlap -- checked at compile time just below).
 */
#define SPLC_FLASH_RETAIN_BASE_ADDR     (SPLC_FLASH_END_ADDR - SPLC_FLASH_RESERVED_SECTORS * SPLC_FLASH_SECTOR_SIZE)
#define SPLC_FLASH_RETAIN_TOTAL_SIZE    (SPLC_FLASH_RETAIN_SECTOR_COUNT * SPLC_FLASH_SECTOR_SIZE) /* 24 KB */

#ifndef __cplusplus
_Static_assert(SPLC_FLASH_RETAIN_BASE_ADDR + SPLC_FLASH_RETAIN_TOTAL_SIZE == SPLC_FLASH_RULE_TABLE_B_ADDR,
               "Retain region must end exactly where Rule Table B begins");
_Static_assert(SPLC_FLASH_RETAIN_BASE_ADDR == SPLC_FLASH_DATA_BASE_ADDR,
               "Retain region must start at the bottom of the reserved data region");
#endif

/*
 * Retain record layout, per docs/SimplePLC_RuleStruct_MCU_Spec_v0.1.md
 * section 7.1's RetainSnapshotHeader, with the per-tag entry count
 * updated from that spec's original 16 (v0.1's VREG_RETAIN range) to
 * v1.9's 32 (see board/board_tag_define.h, VREG_RETAIN0..31):
 *
 *   struct {
 *       uint32_t seq_num;
 *       uint16_t count;
 *       uint16_t crc16;
 *       // followed by: count x { uint16_t tag_index; int32_t value; }
 *   };
 *
 * These are size/count constants only -- services/plc_retain.c (not yet
 * written) is where the actual struct and read/write logic belong; this
 * file intentionally does not declare a struct, to keep this a pure
 * "where and how big" memory-map header that any layer can include
 * without pulling in Layer 3 business logic.
 */
#define SPLC_RETAIN_TAG_COUNT           32U   /* v1.9 VREG_RETAIN range size */
#define SPLC_RETAIN_HEADER_SIZE         8U    /* seq_num(4) + count(2) + crc16(2) */
#define SPLC_RETAIN_ENTRY_SIZE          6U    /* tag_index(2) + value(4), per retained tag */

/*
 * BUGFIX (found on real hardware, confirmed via STM32CubeProgrammer's
 * raw SWD read -- bypasses ICACHE, so this is the true Flash content,
 * not a stale-cache artifact): the natural record size
 * (SPLC_RETAIN_HEADER_SIZE + SPLC_RETAIN_ENTRY_SIZE * SPLC_RETAIN_TAG_COUNT)
 * is 200 bytes, which is NOT a multiple of 16.
 *
 * STM32H5's HAL_FLASH_Program() only supports FLASH_TYPEPROGRAM_QUADWORD
 * (16-byte-aligned writes) -- there is no smaller program granularity on
 * this part. retain_record_addr() places record N at
 * sector_base + N * SPLC_RETAIN_RECORD_SIZE, so with a 200-byte record,
 * every ODD-numbered slot (1, 3, 5, ...) starts 8 bytes off a 16-byte
 * boundary (200 % 16 == 8). sx_flash_write() (platforms/stm32/stm32h5/
 * flash/stm32h5_flash.c) walks the buffer in 16-byte quad-words starting
 * from that misaligned address, so EVERY quad-word it programs for that
 * record lands on a misaligned address and HAL_FLASH_Program() rejects
 * all of them (observed on real hardware: 13/13 quad-words of the
 * affected record failed, logged as repeated
 * "HAL_FLASH_Program failed ... status=1" -- not a partial/random
 * failure, and not caused by the destination not being erased: a raw
 * SWD read of the failing address showed it was still cleanly erased
 * (0xFF) right up until the misalignment kicked in). Even-numbered slots
 * (0, 2, 4, ...) always happened to land on a 16-byte boundary again
 * (200 * 2 = 400 = 16 * 25), which is why slot 0 wrote fine and only
 * every second write ever failed -- easy to miss in short test runs.
 *
 * This was invisible in normal testing because it only bites once
 * retain_snapshot_write() has been called enough times to reach an odd
 * slot index -- with RETAIN_SNAPSHOT_PERIOD_MS's default of 5 minutes,
 * that only surfaces after the device has been left running
 * uninterrupted for 5+ minutes, which most short manual test sessions
 * never reach.
 *
 * Fix: round SPLC_RETAIN_RECORD_SIZE itself up to the next 16-byte
 * multiple (200 -> 208). Every record slot is then a whole multiple of
 * 16 bytes apart, so slot N's address (sector_base + N * 208) is always
 * 16-byte aligned for every N, not just even ones. The extra 8 bytes are
 * unused padding at the end of every record (never read: every read
 * path in plc_retain.c bounds its use of a record by `count`, the
 * header, or the CRC region -- none of them iterate past
 * RETAIN_RECORD_ENTRIES_OFFSET + count * SPLC_RETAIN_ENTRY_SIZE). No
 * other change is required anywhere else: services/plc_retain/
 * plc_retain.c already computes every record address, buffer size, and
 * CRC span purely from SPLC_RETAIN_RECORD_SIZE (see that file's own
 * header comment), so a single-point fix here is sufficient -- verified
 * by grep across plc_retain.c/.h for every use of this constant.
 *
 * Trade-off: SPLC_RETAIN_RECORDS_PER_SECTOR drops from 40 to 39 (8 KB /
 * 208, floored) -- a ~2.5% reduction in usable retain history per
 * sector, well within the Flash-endurance budget this file's header
 * comment already computes (~10 years) for RETAIN_SNAPSHOT_PERIOD_MS's
 * default 5-minute period.
 */
#define SPLC_RETAIN_RECORD_SIZE         (((SPLC_RETAIN_HEADER_SIZE + SPLC_RETAIN_ENTRY_SIZE * SPLC_RETAIN_TAG_COUNT) + 15U) & ~15U) /* 208 bytes (200 rounded up to a 16-byte/quad-word multiple -- see bugfix comment above) */
#define SPLC_RETAIN_RECORDS_PER_SECTOR  (SPLC_FLASH_SECTOR_SIZE / SPLC_RETAIN_RECORD_SIZE) /* 39 */
#define SPLC_RETAIN_TOTAL_RECORDS       (SPLC_RETAIN_RECORDS_PER_SECTOR * SPLC_FLASH_RETAIN_SECTOR_COUNT) /* 117 */

/*
 * Rule Table A/B on-Flash record layout, per
 * services/plc_rule_flash/plc_rule_flash.h:
 *
 *   struct {
 *       uint32_t seq_num;
 *       uint16_t rule_count; // bit 15 = "FB section present" flag,
 *                             // bit 14 = "FB section is the V2 128-byte
 *                             //   image" (only meaningful with bit 15;
 *                             //   clear = the legacy 112-byte image),
 *                             // bits 0..13 = the real rule count
 *       uint16_t crc16;      // field-embedded CRC-16/MODBUS over the
 *                             // whole record (this field itself zeroed
 *                             // during calculation), NOT the same scope
 *                             // as rule_table_wire_crc16()'s own CRC --
 *                             // see plc_rule_flash.h for why.
 *       // followed by: rule_count x 32-byte wire image, via
 *       // rule_record_to_wire() (plc_modbus_cfg.h)
 *       // then, only if the FB flag is set: the Function Block config
 *       // image (plc_fb_export(): 128 bytes, or 112 bytes in a record
 *       // written before Step 8d), covered by the same crc16.
 *   };
 *
 * As with the Retain record layout above, this file intentionally does
 * not declare the actual struct -- only size constants -- to stay a pure
 * "where and how big" memory-map header. SPLC_RULE_FLASH_RECORD_MAX_SIZE
 * is the upper bound at MAX_RULES rules (100 by default); actual on-Flash records
 * are usually smaller (SPLC_RULE_FLASH_HEADER_SIZE + rule_count * 32
 * [+ the FB image size if the FB flag is set]) and
 * services/plc_rule_flash/plc_rule_flash.c reads/writes exactly that
 * many bytes, not this fixed maximum.
 */
#define SPLC_RULE_FLASH_HEADER_SIZE       8U    /* seq_num(4) + rule_count(2) + crc16(2) */
#define SPLC_RULE_FLASH_RECORD_WIRE_SIZE  32U   /* one rule's wire image, matches SPLC_RuleRecord's Modbus layout */
/* MAX_RULES is the product option from config/splc_opt.h (reached through
 * sx_platform_config.h, which this file already includes), not from Layer 2:
 * a Flash memory map still needs nothing from core/. plc_rule_flash.c
 * cross-checks the resulting size with a compile-time static assertion. */
#define SPLC_RULE_FLASH_FB_SIZE           128U  /* FB config image written by Step 8d firmware (plc_fb.h PLC_FB_FLASH_SIZE), cross-checked in plc_rule_flash.c */
#define SPLC_RULE_FLASH_FB_SIZE_V1        112U  /* Legacy image (before Step 8d: no Timer rule binding), read-only (plc_fb.h PLC_FB_FLASH_SIZE_V1) */
#define SPLC_RULE_FLASH_FB_FLAG           0x8000U /* bit 15 of the rule_count field: FB section present */
#define SPLC_RULE_FLASH_FB_V2_FLAG        0x4000U /* bit 14 of the rule_count field: the FB section is the 128-byte image (else 112) */
#define SPLC_RULE_FLASH_COUNT_MASK        0x3FFFU /* bits 0..13 of the rule_count field: the real count */
#define SPLC_RULE_FLASH_RECORD_MAX_SIZE   (SPLC_RULE_FLASH_HEADER_SIZE + (uint32_t)MAX_RULES * SPLC_RULE_FLASH_RECORD_WIRE_SIZE + SPLC_RULE_FLASH_FB_SIZE) /* 3336 bytes at MAX_RULES = 100 */

#endif // STM32H5_PLATFORM

#ifdef __cplusplus
}
#endif

#endif /* SPLC_FLASH_DEFINE_H */