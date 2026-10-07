#ifndef PLC_SYSTEM_CLEAR_H
#define PLC_SYSTEM_CLEAR_H

/*
 * plc_system_clear.h - Layer 4 (Engine & Application entry)
 *
 * Data-clearing half of SYSTEM_COMMAND (0x0A00): CLEAR_RULES, CLEAR_RETAIN
 * and FACTORY_RESET. Split out of plc_system_cmd_service.c on purpose: that
 * file calls sx_system_reset() (Layer 0/1) and can only be built for the
 * ARM target, while everything here goes through Layer 2/3 APIs only
 * (plc_rule.h, plc_tag.h, plc_rule_flash.h, plc_retain.h), so it can be
 * compiled and tested on a plain PC with stub Flash functions.
 *
 * Scope (decided with the project owner, handoff 2.3 step 6): the Flash of
 * this board holds exactly two things -- the Active Rule Table (sectors
 * A/B) and the Retain store. FACTORY_RESET = CLEAR_RETAIN + CLEAR_RULES.
 * Nothing else is touched (no tag layout, no device identity).
 *
 * Mechanism: NOT a raw Flash sector erase. Both stores are cleared by
 * writing a NEW, valid, empty record through the existing, verified
 * save path (plc_rule_flash_save() / retain_snapshot_write()):
 *   - a power loss in the middle leaves either the complete old data or
 *     the complete new empty data, never a half-cleared mix (erasing
 *     sector A and B one after the other would let the not-yet-erased
 *     copy "resurrect" the old rules on the next boot);
 *   - the write is read back and CRC-checked by those functions already.
 * Old values stay physically in older Flash records (logical clear, not
 * a secure erase); the firmware only ever reads the newest valid record.
 *
 * Failure contract: if the Flash write cannot be verified, RAM is put
 * back to what it was before the call and false is returned, so
 * "ERROR" means "not cleared" (RAM keeps the old data).
 *
 * Known limit (pre-existing, not introduced here): if the Flash hardware
 * cannot be programmed AT ALL while erases still work, the first step of
 * plc_rule_flash_save() (copy A -> B = erase B, write B) already destroys
 * the stored rule copies before the failure is noticed. The same happens
 * for any failed rule COMMIT. RAM is still restored here, but after a
 * reboot the rule table could then be empty. A single unwritable sector
 * (the case the A/B scheme is designed for) and a power loss at any point
 * are handled: the result is always all-old or all-empty (PC test).
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Empties the Active Rule Table in RAM and persists the empty table to
 * Flash A+B. Runtime state of all rules is reset (rule_table_commit()
 * does that). Returns true only if the empty table is verified in Flash.
 */
bool plc_clear_rules(void);

/*
 * Sets every TAG_VREG_RETAIN tag to 0 and persists a new all-zero retain
 * record. Returns true only if that record is verified in Flash.
 */
bool plc_clear_retain(void);

/*
 * CLEAR_RETAIN then CLEAR_RULES. Both are always attempted, even if the
 * first one fails, so one bad Flash region does not block clearing the
 * other. Returns true only if BOTH succeeded.
 */
bool plc_factory_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* PLC_SYSTEM_CLEAR_H */