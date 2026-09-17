#ifndef SPLC_FLASH_DEFINE_H
#define SPLC_FLASH_DEFINE_H

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
 *   31 (last)         | 0x03E000                | Rule Table (Active)
 *   30 (2nd-to-last)  | 0x03C000                | Retain slot 1 of 4
 *   29 (3rd-to-last)  | 0x03A000                | Retain slot 2 of 4
 *   28 (4th-to-last)  | 0x038000                | Retain slot 3 of 4
 *   27 (5th-to-last)  | 0x036000                | Retain slot 4 of 4
 *
 * Rule Table: 1 sector (8 KB) holds MAX_RULES(100) * sizeof(SPLC_RuleRecord)(32)
 * = 3200 bytes actually used (39% of the sector) -- see core/plc_rule/plc_rule.h.
 * No wear-leveling here: the Active Rule Table only changes when the App
 * commits a new rule set (rare, human-triggered), unlike Retain which
 * can change every scan cycle -- see docs/architecture.md section 2.6.2
 * for the commit protocol (staging happens over Modbus registers in
 * RAM/App side; only the final committed table is written here).
 *
 * Retain: 4 contiguous sectors (32 KB) using the rotating EEPROM-emulation
 * scheme from docs/SimplePLC_RuleStruct_MCU_Spec_v0.1.md section 7,
 * updated for v1.9's 32-slot VREG_RETAIN range (the v0.1 spec's own
 * numbers -- 104 byte/record, ~78 records/sector, 64 KB/8 sectors -- were
 * computed for the OLD 16-slot VREG_RETAIN layout and do not apply
 * as-is; see the recomputed constants below).
 *
 * Endurance at these 4 sectors, 32 retain slots, RETAIN_SNAPSHOT_PERIOD_MS
 * = 5 minutes (docs/SimplePLC_RuleStruct_MCU_Spec_v0.1.md section 7.1):
 * ~15 years before exhausting the ~10,000 erase-cycle endurance typically
 * quoted for STM32H5 Flash sectors -- accepted as sufficient for this
 * product's expected service life (see chat discussion; no consumer
 * gateway device is expected to still be the same physical unit in active
 * use after 10+ years). If RETAIN_SNAPSHOT_PERIOD_MS is ever configured
 * shorter over Modbus (docs/architecture.md's config knob), this
 * endurance budget shrinks proportionally -- worth re-checking before
 * allowing very short periods.
 */

#include <stdint.h>

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

/* --- Rule Table (Active) -------------------------------------------------
 *
 * Last sector in Flash (global sector #31). Holds the committed Active
 * Rule Table that core/plc_rule/plc_rule.c's rule_table_load_from_flash()
 * reads at boot -- see docs/architecture.md section 2.6.2 for the
 * staging/commit protocol that produces what eventually gets written
 * here.
 */
#define SPLC_FLASH_RULE_TABLE_ADDR      (FLASH_BASE + 0x03E000U)
#define SPLC_FLASH_RULE_TABLE_SIZE      SPLC_FLASH_SECTOR_SIZE   /* 1 sector, 8 KB */

/* --- Retain (VREG_RETAIN rotating EEPROM-emulation store) ---------------
 *
 * 4 contiguous sectors immediately before the Rule Table sector (global
 * sectors #27..#30), per docs/SimplePLC_RuleStruct_MCU_Spec_v0.1.md
 * section 7.1's rotating-record scheme: services/plc_retain.c (not yet
 * written) scans this whole region at boot for the record with the
 * highest seq_num and a valid CRC to find the "active" write position,
 * rather than persisting a separate pointer anywhere.
 *
 * SPLC_FLASH_RETAIN_BASE_ADDR is the LOWEST address of the 4-sector
 * region (sector #27, 5th-from-last) -- i.e. writing/scanning proceeds
 * from SPLC_FLASH_RETAIN_BASE_ADDR upward through
 * SPLC_FLASH_RETAIN_BASE_ADDR + SPLC_FLASH_RETAIN_TOTAL_SIZE - 1, which
 * ends exactly where SPLC_FLASH_RULE_TABLE_ADDR begins (no gap, no
 * overlap -- verified by construction: 0x036000 + 0x8000 == 0x03E000).
 */
#define SPLC_FLASH_RETAIN_BASE_ADDR     (FLASH_BASE + 0x036000U)
#define SPLC_FLASH_RETAIN_SECTOR_COUNT  4U
#define SPLC_FLASH_RETAIN_TOTAL_SIZE    (SPLC_FLASH_RETAIN_SECTOR_COUNT * SPLC_FLASH_SECTOR_SIZE) /* 32 KB */

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
#define SPLC_RETAIN_RECORD_SIZE         (SPLC_RETAIN_HEADER_SIZE + SPLC_RETAIN_ENTRY_SIZE * SPLC_RETAIN_TAG_COUNT) /* 200 bytes */
#define SPLC_RETAIN_RECORDS_PER_SECTOR  (SPLC_FLASH_SECTOR_SIZE / SPLC_RETAIN_RECORD_SIZE) /* 40 */
#define SPLC_RETAIN_TOTAL_RECORDS       (SPLC_RETAIN_RECORDS_PER_SECTOR * SPLC_FLASH_RETAIN_SECTOR_COUNT) /* 160 */

#ifdef __cplusplus
}
#endif

#endif /* SPLC_FLASH_DEFINE_H */