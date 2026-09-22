#ifndef PLC_RULE_FLASH_H
#define PLC_RULE_FLASH_H

/*
 * plc_rule_flash.h - Layer 3 (PLC Application Services)
 *
 * Persists the Active Rule Table (core/plc_rule/plc_rule.c's
 * g_rule_table[]) across power loss, using two whole Flash sectors (A/B)
 * -- see app/splc_flash_define.h for their addresses. Kept as its own
 * file rather than folded into plc_retain.c (different record format and
 * purpose -- see plc_retain.h's updated comment) or plc_modbus_cfg.c
 * (already large; this is one self-contained state machine).
 *
 * --- A/B mechanism (decided with the user, see docs/handoff.md section 1.2) -
 *
 * Steady state: A and B hold the SAME valid rule table. A is the
 * "running" copy (the one plc_rule_flash_load() reads at boot); B is a
 * backup, always kept in sync with the last-known-good A.
 *
 * On plc_rule_flash_save() (called right after rule_table_commit()
 * succeeds in RAM, from plc_modbus_cfg.c's write_commit_command()):
 *   1. Copy A (old) -> B          -- guarantees B is the newest backup of
 *                                     what's currently running, in case
 *                                     step 2/3 below fails partway.
 *   2. Erase A, write the new rule table (already committed in RAM) to A.
 *   3. Read A back, check CRC:
 *        - OK    -> re-sync B = A (erase B, copy A -> B), so B is ready
 *                    as the backup for the next save.
 *        - BAD   -> (power loss mid-write, etc.) report SPLC_ERROR_FLASH,
 *                    copy B -> A to restore A to a runnable state (B was
 *                    never touched on this branch, so it is always intact).
 *
 * At every single step above, a power loss leaves at least one of A/B
 * holding a valid table -- see docs/handoff.md section 1.2's step table
 * for the full walkthrough. No third scratch region is needed.
 *
 * --- On-Flash record format (one copy, written identically to A and B) -
 *
 *   offset 0..3   uint32_t seq_num   (see below)
 *   offset 4..5   uint16_t rule_count
 *   offset 6..7   uint16_t crc16
 *   offset 8..    rule_count x 32-byte WIRE image, via rule_record_to_wire()
 *                 (plc_modbus_cfg.c) -- the exact same per-record byte
 *                 layout ACTIVE_RULE_TABLE/STAGING_RULE_TABLE use over
 *                 Modbus, NOT the in-RAM SPLC_RuleRecord struct layout.
 *
 * crc16 is a field-embedded CRC-16/MODBUS computed over the WHOLE record
 * above -- seq_num + rule_count + the rule_count x 32 wire bytes -- with
 * the crc16 field itself treated as 0 during the calculation (same
 * convention plc_retain.c already uses: zero the field, hash the buffer,
 * restore the field). This is a DELIBERATELY DIFFERENT scope from
 * rule_table_wire_crc16()'s own CRC (which only covers the rule_count x 32
 * data bytes, no header) -- both use the identical CRC-16/MODBUS
 * algorithm (same polynomial/init value, via plc_modbus_cfg.h's
 * crc16_modbus_update()), just over a different byte range, so this is
 * NOT a third CRC formula (docs/handoff.md section 1.4/4.2's warning is
 * about inventing a different algorithm, not about the range it's applied
 * to). The wider range here also protects rule_count itself: a header-only
 * corruption (e.g. rule_count flipped by a bit) would otherwise go
 * undetected by a CRC that only covers the data bytes, and a wrong
 * rule_count would then make this code read the wrong number of trailing
 * bytes as rule data.
 *
 * seq_num increments by 1 on every successful plc_rule_flash_save() (i.e.
 * once per successful Modbus commit), and is written identically to both
 * A and B in that same save -- so in steady state A and B always carry
 * the same seq_num. It is NOT used to pick which of A/B is authoritative
 * (that is always decided by A's own CRC, per the mechanism above): its
 * purpose is purely a monotonically increasing counter for logging/
 * diagnostics (e.g. cross-checking against ACTIVE_RULE_VERSION over
 * Modbus, or telling apart A/B by eye when inspecting Flash by hand).
 *
 * Record size varies with rule_count (8-byte header + rule_count x 32),
 * NOT a fixed 3208 bytes (the MAX_RULES=100 upper bound) -- this file
 * reads/writes exactly header_size + rule_count * 32 bytes, so a small
 * rule table does not need to scan a full sector's worth of (mostly
 * padding) data. sx_flash_write() itself still rounds the actual Flash
 * program operation up to the next 16-byte quad-word internally (padding
 * with 0xFF) -- that padding is never read back by this file, since reads
 * are always bounded by header.rule_count, not by the padded write size.
 *
 * This file may include Layer 0/1 headers (sx_flash.h) -- Layer 3 is not
 * required to build/test hardware-free, unlike Layer 2 (core/plc_rule/
 * plc_rule.c only ever receives an already-read, already-validated buffer
 * from this file; it must never itself call sx_flash_*()).
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Loads the Active Rule Table from Flash sector A (falling back to B and
 * self-healing A if A's CRC is bad -- see the A/B mechanism above), then
 * calls rule_table_commit() (Layer 2) with what it finds so
 * g_rule_table[]/g_rule_runtime[] end up populated exactly as if the App
 * had just staged and committed that same data over Modbus.
 *
 * If NEITHER A nor B holds a record with a valid CRC (first boot ever, or
 * both sectors blank/corrupted), this leaves the rule table at whatever
 * rule_table_load_from_flash() (Layer 2, called just before this in
 * plc_engine_init()) already initialized it to -- an empty table
 * (rule_count = 0) -- rather than treating that as a fatal error. A
 * brand-new device with no rules ever configured is an expected,
 * non-error startup state.
 *
 * Called exactly once at boot, from plc_engine_init() (Layer 4), in place
 * of Layer 2's own now-Flash-free rule_table_load_from_flash() call --
 * see docs/architecture.md section 4's plc_engine_init() ordering and
 * docs/handoff.md section 1.3's file-by-file plan. Must run AFTER
 * rule_table_load_from_flash() (so g_rule_table[]/g_rule_runtime[] are
 * already in the defined empty state this function's own "not found"
 * path relies on) and AFTER tag_table_load_from_flash() (rule_table_
 * commit() itself has no tag-table dependency today, but this keeps the
 * same relative ordering Layer 2's rule/tag load calls already had).
 */
void plc_rule_flash_load(void);

/*
 * Persists the CURRENT in-RAM Active Rule Table (g_rule_table[] /
 * g_rule_count.rule_count, core/plc_rule/plc_rule.c) to both Flash
 * sectors A and B, following the A/B mechanism above.
 *
 * Called from plc_modbus_cfg.c's write_commit_command(), immediately
 * after rule_table_commit() (Layer 2) has already applied the new table
 * to RAM successfully -- per docs/handoff.md section 1.1: Flash is
 * written as part of the SAME commit, not a separate SYSTEM_COMMAND step,
 * matching the original spec's step 4 ("atomic-swap + save Flash +
 * increment version" as one step). Must be called BEFORE
 * write_commit_command() sets CONFIG_STATUS/increments ACTIVE_RULE_VERSION
 * / logs the result, so the App's single post-commit CONFIG_STATUS read
 * only ever observes a fully-finished state (RAM and Flash both done,
 * with no App-visible "RAM done, Flash pending" intermediate state) -- see
 * docs/handoff.md section 1.1 point 4.
 *
 * Returns true ONLY if A ends up holding a verified-good copy of the NEW
 * table written by THIS call. Returns false in two distinct failure
 * cases, both meaning "the new table did not survive to Flash, but A is
 * not necessarily broken":
 *   - The direct write to A failed its read-back CRC check, but restoring
 *     A from B (the previous good table) succeeded -- A now holds the
 *     PREVIOUS table again, not the new one.
 *   - Both the direct write to A and the B->A restore failed to produce a
 *     valid CRC on read-back (extremely unlikely short of a failing/worn
 *     Flash sector) -- A's on-Flash state is now unknown/unreliable.
 * The rule table ALREADY RUNNING in RAM is unaffected by this function's
 * return value in either case -- rule_table_commit() has already
 * succeeded by the time this is called, so the device keeps running the
 * NEW rules regardless of what happened to Flash; a false return means
 * only that they failed to survive a future reset (the device would boot
 * back into either the previous table or, in the second case, whatever
 * plc_rule_flash_load() can still recover, possibly an empty table).
 *
 * Caller contract on failure (per docs/handoff.md section 1.1 point 5):
 * the caller must still report CONFIG_STATUS = READY (the rule table IS
 * running) together with CONFIG_ERROR_CODE = SPLC_ERROR_FLASH (it did NOT
 * survive being saved) -- never CONFIG_STATUS = ERROR for this case, since
 * that would incorrectly tell the App the commit itself failed and the
 * old rules are still active.
 */
bool plc_rule_flash_save(void);

#ifdef __cplusplus
}
#endif

#endif /* PLC_RULE_FLASH_H */