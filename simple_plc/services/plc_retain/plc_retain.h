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
 * has its own separate two-sector (A/B) Flash region and its own load
 * path (services/plc_rule_flash/plc_rule_flash.c's plc_rule_flash_load(),
 * see that file for the A/B recovery mechanism) with no wear-leveling
 * within either sector, since rule commits are rare and human-triggered
 * while retain snapshots are periodic and automatic. See
 * docs/architecture.md section 3.2.
 *
 * This file may include Layer 0/1 (sx_flash.h, sx_pwd.h, sx_time.h) --
 * unlike Layer 2, Layer 3 is not required to build/test hardware-free.
 * It also uses nanoMODBUS's own nmbs_crc_calc() (libs/nanomodbus/
 * nanomodbus.h) for the record's CRC-16/MODBUS rather than a separate
 * project CRC implementation -- nanoMODBUS already needs this exact
 * algorithm for RTU framing, so reusing it avoids a second
 * hand-maintained copy of the same CRC-16/MODBUS polynomial/init value.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * RETAIN_SNAPSHOT_PERIOD_MS, PLC_PVD_EMERGENCY_SAVE_ENABLE and
 * RETAIN_EMERGENCY_MIN_INTERVAL_MS are product options: set them in the
 * product's splcopts.h (meaning, default and range in config/splc_opt.h).
 * Shortening the snapshot period spends Flash endurance (the estimate in
 * flash_define assumes the 5 minute default); re-check it before changing.
 */
#include "splc_opt.h"

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
 * (via sx_get_tick_ms(), Layer 1) and, once RETAIN_SNAPSHOT_PERIOD_MS has
 * elapsed, writes a snapshot ONLY IF a retain tag differs from the newest
 * record in Flash (the PVD interrupt is the primary power-loss path; this
 * is the safety net, and an unchanged period costs no Flash write). Also
 * performs a PVD write that the interrupt had to defer because the main
 * loop was inside a Flash operation. Cheap to call every scan cycle when
 * nothing is due.
 */
void retain_service(void);

/*
 * Writes one new retain record capturing the current value of every
 * TAG_VREG_RETAIN tag, unconditionally (used by CMD_COMMIT_RETAIN and the
 * clear commands, which must always write). MAIN LOOP ONLY: it may erase
 * a sector and takes as long as the Flash needs. The power-loss path is
 * retain_emergency_snapshot() below, not this function.
 *
 * Returns true only if the record was read back from Flash with a valid
 * CRC and the expected seq_num (Wire Contract section 7 step 3: commit is
 * verified). false = write position unknown, or the read-back check
 * failed; the slot is still consumed (a partly programmed slot cannot be
 * reused), and the previous record stays the newest valid one, so a failed
 * write never loses data already saved. retain_service() ignores the
 * result; CMD_COMMIT_RETAIN (plc_modbus_cfg.c) uses it.
 *
 * Uses a monotonically increasing seq_num so retain_store_restore() can
 * always identify the most recent record. When a write fills the active
 * sector, the NEXT sector in the ring is erased immediately (pre-erase),
 * so the following write -- possibly the PVD one -- never has to erase.
 * A slot that is not blank (earlier interrupted write) is skipped.
 */
bool retain_snapshot_write(void);

/*
 * PVD / low-voltage emergency write. Meant to be registered with
 * sx_power_register_low_voltage_callback() by Layer 4 (plc_engine_init()
 * does, when PLC_PVD_EMERGENCY_SAVE_ENABLE is 1). Runs in INTERRUPT
 * context, so it:
 *   - never logs and never erases (programs with sx_flash_write_quiet());
 *   - does nothing while the main loop is inside a Flash operation
 *     (retain_flash_op_begin()..end()): it only raises a flag and
 *     retain_service() writes later, when the Flash is free;
 *   - does nothing if no retain tag changed since the last saved record;
 *   - does nothing if called again within RETAIN_EMERGENCY_MIN_INTERVAL_MS
 *     of the previous emergency write;
 *   - defers to the main loop if the active sector is full (an erase is
 *     needed -- the pre-erase makes this rare) or the write did not verify.
 * Uses about 0.6 KB of stack (record buffer + tag snapshot).
 */
void retain_emergency_snapshot(void);

/*
 * Flash-operation guard for the PVD path. Every MAIN-LOOP code path that
 * erases or programs Flash outside this file must wrap the operation in
 * begin/end so the PVD interrupt will not start a second Flash operation
 * on top of it (the HAL would return BUSY, and the interrupt must not
 * touch the retain write position mid-update). plc_rule_flash.c does this
 * around its erase+write. Calls nest; never call from interrupt context.
 */
void retain_flash_op_begin(void);
void retain_flash_op_end(void);

#ifdef __cplusplus
}
#endif

#endif /* PLC_RETAIN_H */