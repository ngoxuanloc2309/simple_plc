#ifndef PLC_TAGTYPE_H
#define PLC_TAGTYPE_H

/*
 * plc_tagtype.h - Layer 3 (PLC Application Services)
 *
 * Tag DataType Registry: which tags hold a BOOL, an INT32 or a REAL (IEEE-754
 * float32). The tag store itself stays an int32_t array (g_tag_value[]); a REAL
 * tag simply carries the 32 float bits in that slot. Only the Host knows the
 * type of a tag (it is a property of the project's Tag Catalog), so it writes
 * this table to the MCU when it deploys (App contract changelog 2.9.0, Structs
 * v2.0 section 4.3, decision 40 in docs/handoff.md).
 *
 * Wire (Modbus, FC03 / FC06 / FC16, 64 registers = 128 bytes):
 *   0x0080 + k  (k = 0..63): High byte = type of tag 2k, Low byte = type of tag 2k+1
 *   type byte: 0 = not declared (treated as INT32), 1 = BOOL, 2 = INT32, 3 = REAL
 *
 * Staging (same idea as plc_mqtt): Host writes do NOT touch the running table.
 * They go to a DRAFT (copied from the running table on the first write, so a
 * partial write keeps the rest). The running table -- what FC03 and every user
 * of plc_tagtype_get() see -- only changes at COMMIT:
 *     plc_tagtype_validate_draft() : false = reject the COMMIT, nothing changed
 *     plc_tagtype_commit_draft()   : the draft becomes the running table
 *     plc_tagtype_discard_draft()  : drop the draft
 * A Deploy that does not write the table keeps the running one. The table is
 * saved in the Flash Rule Table record (flag bit 11, decision 41), so the
 * types survive a power cycle together with the rules that depend on them.
 *
 * Rules checked at COMMIT / import:
 *   - every byte is 0..3;
 *   - REAL only on tags whose kind can hold a number from the field: AI, VREG,
 *     VREG_RETAIN and MB_HOLDING (a DI/DO/VFLAG/COUNTER typed REAL is refused).
 *     Entries for tags >= MAX_TAGS do not exist on this board and are ignored.
 *     A slot below MAX_TAGS that no layout group covers (kind NONE, e.g. Gateway
 *     with 112 runtime tags and MAX_TAGS 128) is NOT ignored: REAL there is refused.
 *
 * When a tag changes between an integer type and REAL at COMMIT, its stored
 * value would be reinterpreted as the wrong kind of number, so a VREG,
 * VREG_RETAIN or MB_HOLDING tag is reset to 0 at that moment (an AI tag is
 * rewritten by the next input scan). BOOL <-> INT32 changes keep the value.
 *
 * The Rule Engine does NOT consult this table: a rule's float mode is decided
 * by its own flags bit (App contract). MB_READ, MQTT Publish and MQTT Subscribe
 * (Layer 3) use plc_tagtype_get().
 *
 * Capability: bit 11 (0x0800) of HARDWARE_CAPABILITIES 0x0A08, App header
 * SPLC_CAP_FLOAT32 (confirmed by the App team, handoff decision 55). It is
 * announced whenever SPLC_REAL_ENABLE=1 (see read_hw_capabilities()).
 *
 * Threading: single-threaded, engine context only. Compiled only with
 * SPLC_REAL_ENABLE = 1, so a build with it 0 keeps its exact baseline size.
 */

#include <stdbool.h>
#include <stdint.h>
#include "splc_opt.h"

#if SPLC_REAL_ENABLE

#ifdef __cplusplus
extern "C" {
#endif

#define PLC_TAGTYPE_BASE        0x0080U
#define PLC_TAGTYPE_REGS        64U
#define PLC_TAGTYPE_COUNT       128U   /* tags covered: 2 per register */
#define PLC_TAGTYPE_FLASH_SIZE  128U   /* one byte per tag, in tag order */

#define PLC_TAGTYPE_CAP_FLOAT32      0x00000800UL   /* App header SPLC_CAP_FLOAT32, bit 11 of 0x0A08 */

typedef enum {
    PLC_TAGTYPE_UNDECLARED = 0,
    PLC_TAGTYPE_BOOL       = 1,
    PLC_TAGTYPE_INT32      = 2,
    PLC_TAGTYPE_REAL       = 3
} plc_tagtype_t;

typedef enum {
    PLC_TAGTYPE_WRITE_OK = 0,
    PLC_TAGTYPE_WRITE_BAD_ADDRESS     /* -> Modbus 0x02 */
} plc_tagtype_write_result_t;

/* Boot: every tag undeclared (INT32), draft dropped. Call once, before
 * plc_rule_flash_load(). */
void plc_tagtype_init(void);

/* Type of `tag` in the RUNNING table. An undeclared tag or one outside the
 * table (>= PLC_TAGTYPE_COUNT) is INT32, so the result is always 1, 2 or 3. */
plc_tagtype_t plc_tagtype_get(uint16_t tag);

/* Convenience for the callers that only care about floats. */
bool plc_tagtype_is_real(uint16_t tag);

/* FC03 of `quantity` registers at `offset` (0..63): the RUNNING table packed
 * as on the wire. offset + quantity must stay inside the area. */
void plc_tagtype_read(uint16_t offset, uint16_t quantity, uint16_t *registers_out);

/* FC16 (FC06 arrives as quantity 1) into the DRAFT. A request that runs past
 * the end of the area is BAD_ADDRESS and changes nothing. The content is
 * checked at COMMIT, not here. */
plc_tagtype_write_result_t plc_tagtype_write(uint16_t offset, uint16_t quantity,
                                             const uint16_t *registers);

bool plc_tagtype_validate_draft(void);   /* true also when nothing was written */
void plc_tagtype_commit_draft(void);     /* call only after validate returned true */
void plc_tagtype_discard_draft(void);

/* Flash image: PLC_TAGTYPE_FLASH_SIZE bytes, byte i = type of tag i (the RUNNING
 * table, never the draft). */
void plc_tagtype_export(uint8_t *out);

/* Load an image written by plc_tagtype_export(). The whole image is checked
 * with the COMMIT rules; if it fails NOTHING is applied and false is returned.
 * Used at boot and to roll back a failed CLEAR_RULES. Tag values are not
 * touched (this is a restore, not a type change). Drops any draft. */
bool plc_tagtype_import(const uint8_t *in);

/* CLEAR_RULES / FACTORY_RESET: every tag undeclared, draft dropped. Tag values
 * are not touched (CLEAR_RULES does not reset any other tag value either). */
void plc_tagtype_clear_all(void);

#ifdef __cplusplus
}
#endif

#endif /* SPLC_REAL_ENABLE */
#endif /* PLC_TAGTYPE_H */