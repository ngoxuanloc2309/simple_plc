#ifndef PLC_RETAIN_H
#define PLC_RETAIN_H

/*
 * plc_retain.h - Layer 3 (PLC Application Services)
 *
 * Retentive storage for TAG_VREG_RETAIN tag values -- the equivalent of a
 * traditional PLC's "retentive/latching" register group, surviving power
 * loss. Implements the periodic-snapshot EEPROM-emulation scheme from
 * docs/SimplePLC_RuleStruct_MCU_Spec_v0.1.md section 7, with the record
 * layout's per-record tag count updated for v1.9's 32-slot VREG_RETAIN
 * range (see app/splc_flash_define.h, which already fixes every size/
 * address constant this file needs).
 *
 * ONLY for TAG_VREG_RETAIN tags -- NOT shared with the Rule Table, which
 * has its own separate Flash region and its own load path
 * (core/plc_rule/plc_rule.c's rule_table_load_from_flash()) with no
 * wear-leveling, since rule commits are rare and human-triggered while
 * retain snapshots are periodic and automatic. See
 * docs/architecture.md section 3.2.
 *
 * This file may include Layer 0/1 (sx_flash.h, sx_pwd.h, sx_time.h) --
 * unlike Layer 2, Layer 3 is not required to build/test hardware-free.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Snapshot period for the routine (non-emergency) write path, per
 * docs/SimplePLC_RuleStruct_MCU_Spec_v0.1.md section 7.1. Configurable
 * over Modbus in the future (see that section's note) -- for now a fixed
 * compile-time constant. Shortening this reduces the Flash-endurance
 * budget computed in app/splc_flash_define.h's header comment
 * (~15 years at this period); that tradeoff must be re-checked before
 * this value is ever made runtime-configurable.
 */
#define RETAIN_SNAPSHOT_PERIOD_MS (5U * 60U * 1000U)

/*
 * Scans the entire retain Flash region (app/splc_flash_define.h's
 * SPLC_FLASH_RETAIN_BASE_ADDR .. +SPLC_FLASH_RETAIN_TOTAL_SIZE) for the
 * record with the highest seq_num and a valid CRC-16/MODBUS, and loads
 * its {tag_index, value} entries into g_tag_value[] via tag_write() for
 * every TAG_VREG_RETAIN tag it finds.
 *
 * No record found with a valid CRC (e.g. first boot, blank Flash): every
 * TAG_VREG_RETAIN tag is left at its power-on-reset value (0), per
 * docs/SimplePLC_RuleStruct_MCU_Spec_v0.1.md section 7.2 -- this function
 * does not treat that as an error.
 *
 * Called exactly once at boot, from plc_engine_init() (Layer 4), AFTER
 * tag_table_load_from_flash() (so g_tag_table[]'s kinds are already
 * populated and tag_write() can be safely used) -- see
 * docs/architecture.md section 4's plc_engine_init() ordering.
 */
void retain_store_restore(void);

/*
 * Called once per scan cycle (every 10 ms) from plc_engine_scan_once(),
 * AFTER modbus_config_service() -- see docs/architecture.md section 4.2's
 * ordering. Internally tracks elapsed time since the last snapshot write
 * (via sx_get_tick_ms(), Layer 1) and calls retain_snapshot_write() once
 * RETAIN_SNAPSHOT_PERIOD_MS has elapsed. Cheap to call every scan cycle
 * when nothing is due -- the tick comparison is the only work done in
 * that case.
 */
void retain_service(void);

/*
 * Writes one new retain record capturing the current value of every
 * TAG_VREG_RETAIN tag. Exposed as a separate entry point (rather than
 * being purely internal to retain_service()'s periodic path) so the
 * low-voltage/PVD emergency path (see components/pwd/sx_pwd.h's
 * sx_power_register_low_voltage_callback()) can trigger an immediate
 * out-of-cycle write when power loss is imminent, per
 * docs/SimplePLC_RuleStruct_MCU_Spec_v0.1.md section 7.3.
 *
 * Registering the callback that calls this from interrupt context is a
 * Layer 4 responsibility (board/engine init), not this file's -- this
 * function only needs to be safe to call from that context: it must
 * complete a single bounded Flash write and return, with no blocking
 * wait beyond what sx_flash_write() itself does.
 *
 * Uses a monotonically increasing seq_num so retain_store_restore() can
 * always identify the most recent record; wraps the write position
 * forward within the current active sector, erasing and advancing to the
 * next sector in rotation (per app/splc_flash_define.h's
 * SPLC_FLASH_RETAIN_SECTOR_COUNT) when the active sector is full.
 */
void retain_snapshot_write(void);

#ifdef __cplusplus
}
#endif

#endif /* PLC_RETAIN_H */