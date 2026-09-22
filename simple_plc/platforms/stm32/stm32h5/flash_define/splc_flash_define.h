#ifndef SPLC_FLASH_DEFINE_H
#define SPLC_FLASH_DEFINE_H

#include "sx_platform_config.h"

/*
 * app/splc_flash_define.h - Layer 4 (App / product-specific configuration)
 *
 * Flash memory map for this product (Remote I/O, STM32H523CCU6). Defines
 * WHERE each persistent-storage region lives in Flash, as a set of
 * macros -- not a driver, not business logic. Kept in app/ (Layer 4)
 * rather than platforms/ or services/ because this layout is a
 * PRODUCT decision (this SKU, this chip's 256 KB), not a chip-driver
 * concern (that's platforms/stm32/stm32h5/flash/stm32h5_flash.c, which
 * knows HOW to erase/read/write a sector but not WHAT goes in which
 * one) and not a chip-agnostic business-logic concern (that's
 * services/plc_retain.c, which should be portable to a future chip with
 * a different Flash size/layout by swapping only this file).
 *
 * Every stm32h5_*.h file in platforms/stm32/stm32h5/ already #includes
 * sx_platform_config.h from this same app/ directory (see the TODO in
 * platforms/stm32/stm32h5/CMakeLists.txt) to select STM32H5_PLATFORM --
 * this file follows that same existing precedent of product-specific
 * config living in app/ and being reached by lower layers via the
 * PUBLIC include path platforms/stm32/stm32h5/CMakeLists.txt already
 * exposes.
 *
 * Hardware facts this file is built on (STM32H523CCU6, per ST datasheet
 * DS14540 / RM0481, cross-checked against
 * platforms/stm32/stm32h5/flash/stm32h5_flash.c's own verified
 * constants -- see the comment block at the top of that file):
 *   - 256 KB total Flash, dual-bank: 2 banks x 128 KB
 *   - 16 sectors/bank x 8 KB/sector = 32 sectors total, globally
 *     numbered 0..31 (sector 0..15 = Bank 1, sector 16..31 = Bank 2)
 *   - FLASH_BASE (0x08000000) comes from the CMSIS device header via
 *     the stm32cubemx target, not redefined here.
 *
 * Layout decided (5 sectors reserved at the very end of Flash, counting
 * backwards from the last sector):
 *
 *   Sector # (global) | Offset from FLASH_BASE | Region
 *   ------------------+-------------------------+------------------
 *   31 (last)         | 0x03E000                | Rule Table A (running copy)
 *   30 (2nd-to-last)  | 0x03C000                | Rule Table B (backup copy)
 *   29 (3rd-to-last)  | 0x03A000                | Retain slot 1 of 3
 *   28 (4th-to-last)  | 0x038000                | Retain slot 2 of 3
 *   27 (5th-to-last)  | 0x036000                | Retain slot 3 of 3
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
 * 4 sectors to 3 to free one sector (former sector #30) for Rule Table B
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

/* --- Sector geometry (STM32H523CCU6) ------------------------------------
 *
 * Mirrors platforms/stm32/stm32h5/flash/stm32h5_flash.c's own
 * SX_FLASH_TOTAL_SIZE/SX_FLASH_BANK_SIZE constants (not re-derived from
 * CMSIS FLASH_SIZE -- see that file's comment on why that macro is
 * unsafe on this part). Duplicated here rather than #included from
 * there because those are file-local #defines in stm32h5_flash.c, not
 * exposed via stm32h5_flash.h -- if that ever changes, these should
 * become a single shared source of truth instead of two copies that
 * could drift.
 */
#define SPLC_FLASH_SECTOR_SIZE          0x2000U     /* 8 KB, matches FLASH_SECTOR_SIZE (CMSIS) */
#define SPLC_FLASH_TOTAL_SIZE           0x40000U    /* 256 KB total */
#define SPLC_FLASH_SECTOR_COUNT         32U         /* 256 KB / 8 KB */

/* --- Rule Table A/B (see services/plc_rule_flash/plc_rule_flash.h) ------
 *
 * Last two sectors in Flash (global sectors #31 and #30). A is the
 * "running" copy that plc_rule_flash_load() reads first at boot; B is its
 * backup, used to self-heal A if A's CRC is ever bad (e.g. power loss
 * mid-write) -- see plc_rule_flash.h for the full mechanism. Both sectors
 * use the identical on-Flash record format; app/architecture docs refer
 * to this pair collectively as "the Rule Table Flash region". See
 * docs/architecture.md section 2.6.2 for the Modbus staging/commit
 * protocol that produces what eventually gets written here.
 */
#define SPLC_FLASH_RULE_TABLE_A_ADDR    (FLASH_BASE + 0x03E000U)
#define SPLC_FLASH_RULE_TABLE_B_ADDR    (FLASH_BASE + 0x03C000U)
#define SPLC_FLASH_RULE_TABLE_SIZE      SPLC_FLASH_SECTOR_SIZE   /* 1 sector each, 8 KB */

/* --- Retain (VREG_RETAIN rotating EEPROM-emulation store) ---------------
 *
 * 3 contiguous sectors immediately before Rule Table B (global sectors
 * #27..#29), per docs/SimplePLC_RuleStruct_MCU_Spec_v0.1.md section 7.1's
 * rotating-record scheme: services/plc_retain/plc_retain.c scans this
 * whole region at boot for the record with the highest seq_num and a
 * valid CRC to find the "active" write position, rather than persisting
 * a separate pointer anywhere. Reduced from 4 to 3 sectors to free sector
 * #30 for Rule Table B -- see the header comment above.
 *
 * SPLC_FLASH_RETAIN_BASE_ADDR is the LOWEST address of the 3-sector
 * region (sector #27, 5th-from-last) -- i.e. writing/scanning proceeds
 * from SPLC_FLASH_RETAIN_BASE_ADDR upward through
 * SPLC_FLASH_RETAIN_BASE_ADDR + SPLC_FLASH_RETAIN_TOTAL_SIZE - 1, which
 * ends exactly where SPLC_FLASH_RULE_TABLE_B_ADDR begins (no gap, no
 * overlap -- verified by construction: 0x036000 + 0x6000 == 0x03C000).
 */
#define SPLC_FLASH_RETAIN_BASE_ADDR     (FLASH_BASE + 0x036000U)
#define SPLC_FLASH_RETAIN_SECTOR_COUNT  3U
#define SPLC_FLASH_RETAIN_TOTAL_SIZE    (SPLC_FLASH_RETAIN_SECTOR_COUNT * SPLC_FLASH_SECTOR_SIZE) /* 24 KB */

/*
 * Retain record layout, per docs/SimplePLC_RuleStruct_MCU_Spec_v0.1.md
 * section 7.1's RetainSnapshotHeader, with the per-tag entry count
 * updated from that spec's original 16 (v0.1's VREG_RETAIN range) to
 * v1.9's 32 (see core/plc_tag/plc_tag_def.h, VREG_RETAIN0..31):
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
 *       uint16_t rule_count;
 *       uint16_t crc16;      // field-embedded CRC-16/MODBUS over the
 *                             // whole record (this field itself zeroed
 *                             // during calculation), NOT the same scope
 *                             // as rule_table_wire_crc16()'s own CRC --
 *                             // see plc_rule_flash.h for why.
 *       // followed by: rule_count x 32-byte wire image, via
 *       // rule_record_to_wire() (plc_modbus_cfg.h)
 *   };
 *
 * As with the Retain record layout above, this file intentionally does
 * not declare the actual struct -- only size constants -- to stay a pure
 * "where and how big" memory-map header. SPLC_RULE_FLASH_RECORD_MAX_SIZE
 * is the upper bound at MAX_RULES (100) rules; actual on-Flash records
 * are usually smaller (SPLC_RULE_FLASH_HEADER_SIZE + rule_count * 32) and
 * services/plc_rule_flash/plc_rule_flash.c reads/writes exactly that
 * many bytes, not this fixed maximum.
 */
#define SPLC_RULE_FLASH_HEADER_SIZE       8U    /* seq_num(4) + rule_count(2) + crc16(2) */
#define SPLC_RULE_FLASH_RECORD_WIRE_SIZE  32U   /* one rule's wire image, matches SPLC_RuleRecord's Modbus layout */
/* MAX_RULES (100) comes from core/plc_rule/plc_rule.h; not re-included
 * here to keep this file free of Layer 2 dependencies (Layer 4 headers
 * should not need to pull in Layer 2 to read a Flash memory map) --
 * duplicated as a literal, cross-checked against MAX_RULES by a
 * compile-time static assertion in plc_rule_flash.c instead. */
#define SPLC_RULE_FLASH_RECORD_MAX_SIZE   (SPLC_RULE_FLASH_HEADER_SIZE + 100U * SPLC_RULE_FLASH_RECORD_WIRE_SIZE) /* 3208 bytes max */

#endif // STM32H5_PLATFORM

#ifdef __cplusplus
}
#endif

#endif /* SPLC_FLASH_DEFINE_H */