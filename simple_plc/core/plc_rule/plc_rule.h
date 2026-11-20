#ifndef PLC_RULE_H
#define PLC_RULE_H

/*
 * plc_rule.h - Layer 2 (PLC Core)
 *
 * Rule Engine: evaluates a fixed table of rules on every scan cycle and
 * executes an action when a rule's condition is met. This file must not
 * include anything from Layer 0/1; it only depends on plc_tag.h within
 * Layer 2 and standard C headers.
 *
 * See docs/architecture.md, section 2, "Layer 2 - PLC Core".
 */

#include <stdbool.h>
#include <stdint.h>

#include "plc_tag.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum number of rules supported system-wide: product option MAX_RULES
 * (splcopts.h; default and limits in config/splc_opt.h). */
#include "splc_opt.h"

/* Bit position within Rule.guard_tag used to negate the guard condition.
 * guard_tag & GUARD_TAG_NEGATE_BIT set means "fire only when guard is 0".
 * The remaining bits (0-14) hold the actual tag index. */
#define GUARD_TAG_NEGATE_BIT   0x8000u
#define GUARD_TAG_INDEX_MASK   0x7FFFu

/*
 * Sentinel meaning "this rule has no guard at all" -- GUARD_TAG_INDEX_MASK
 * (all 15 index bits set = 0x7FFF = 32767) is used instead of 0.
 *
 * WHY NOT 0: under the v1.9 tag layout (board/board_tag_define.h; DI is
 * always the first group regardless of board, per plc_tag.h's
 * tag_di_base_index()), tag index 0 is
 * TAG_DI0 -- a real, addressable tag, not an unused slot. v1.7 reserved
 * index 0 as a dedicated TAG_NONE sentinel with no physical meaning, so
 * "guard_tag == 0 means no guard" was safe back then. v1.9 removed that
 * reserved slot to fit the fixed wire layout (DI 0-7, DO 8-15, ...), which
 * silently broke that assumption: a rule author who set
 * guard_tag = TAG_DI0 (0) *intending* to gate on DI0's real value would
 * instead have had that guard treated as "absent" and always pass,
 * regardless of DI0's actual level -- DI0 would have been the one tag in
 * the whole system that could never be used as a guard. Confirmed by an
 * actual compiled/run test before this fix: a rule with guard_tag=TAG_DI0
 * still fired even with DI0 held at 0.
 *
 * 0x7FFF is safe as the new sentinel because GUARD_TAG_INDEX_MASK is only
 * 15 bits wide, so it can never equal a real tag index: MAX_TAGS is at most 128
 * (see board/board_tag_define.h; 112 on the Zigbee-IO SKU), far below 32767.
 * Every valid tag index (0..MAX_TAGS-1) is therefore always distinguishable
 * from "no guard".
 *
 * A guard_tag whose index bits fall in MAX_TAGS..32766 (neither a valid tag
 * nor this sentinel) is refused at COMMIT by rule_table_find_bad_tag()
 * (below), which plc_modbus_cfg.c calls before rule_table_commit().
 * rule_table_commit() itself does NOT check it: it also loads the rule table
 * from Flash at boot, and a table stored by an older firmware must still load.
 */
#define GUARD_TAG_NONE         0x7FFFu

/*
 * TriggerType selects which edge/condition on trigger_tag activates a
 * rule's evaluation.
 */
typedef enum {
    SPLC_TRG_ON_CHANGE = 0,  /* Fires whenever current value differs from previous */
    SPLC_TRG_ON_RISE,        /* Fires on a 0 -> nonzero transition */
    SPLC_TRG_ON_FALL,        /* Fires on a nonzero -> 0 transition */
    SPLC_TRG_TIME_WINDOW,     /* Fires while current time falls within a window */
    SPLC_TRG_INTERVAL,        /* Fires periodically at a fixed interval */
} SPLC_TriggerType;


/*
 * CompareOp selects how the current tag value is compared against
 * threshold_lo/threshold_hi before a rule is allowed to fire.
 */
typedef enum {
    SPLC_OP_NONE = 0,
    SPLC_OP_EQ,
    SPLC_OP_NEQ,
    SPLC_OP_GT,
    SPLC_OP_LT,
    SPLC_OP_GTE,
    SPLC_OP_LTE,
    SPLC_OP_BETWEEN,   /* threshold_lo <= current <= threshold_hi */
} SPLC_CompareOp;

/*
 * ActionType selects what execute_action() does when a rule fires.
 */
typedef enum {
    SPLC_ACT_SET_TAG = 0,     /* action_tag = action_param */
    SPLC_ACT_TOGGLE_TAG,      /* action_tag = !action_tag (as boolean) */
    SPLC_ACT_INC_COUNTER,     /* action_tag += action_param */
    SPLC_ACT_WRITE_REMOTE,    /* Flag a pending write to a remote Modbus tag */
    SPLC_ACT_LOG_EVENT,       /* Append an entry to the internal RAM event log */
    SPLC_ACT_SEND_ALARM,      /* Set an alarm code into a dedicated tag */
    SPLC_ACT_ADD_TAG,         /* action_tag += tag_read(trigger_tag) */
    SPLC_ACT_SCALE_TAG,       /* action_tag = tag_read(trigger_tag) * action_param / 1000 + threshold_hi
                                 (float rule: x * action_param_f + threshold_hi_f) */
    SPLC_ACT_MATH_OP,         /* 8: action_tag = Op1 <compare_op> Op2 (SPLC-SPEC-MATH-007), runs every
                                 scan while enabled; only executed when SPLC_MATH_ENABLE=1 */
} SPLC_ActionType;


typedef struct {
    int32_t  threshold_lo;
    int32_t  threshold_hi;
    uint32_t for_ms;          /* Dwell/debounce duration in milliseconds */
    int32_t  action_param;
    uint16_t trigger_tag;     /* Tag index used as the trigger condition */
    uint16_t action_tag;      /* Tag index affected by the action */
    uint16_t guard_tag;       /* Packed guard tag index (bits 0-14) + NEGATE
                                 bit (bit 15). Index bits == GUARD_TAG_NONE
                                 (0x7FFF) means "no guard" -- NOT 0, since 0
                                 is the real index of TAG_DI0 under the v1.9
                                 tag layout. See GUARD_TAG_NONE above. */
    uint8_t  enabled;
    uint8_t  trigger_type;    /* One of SPLC_TriggerType */
    uint8_t  compare_op;      /* One of SPLC_CompareOp */
    uint8_t  action_type;     /* One of SPLC_ActionType */
#if SPLC_REAL_ENABLE
    uint8_t  flags;           /* SPLC_RULE_FLAG_* bits (wire reg 13, high byte). Exists, and is carried on the wire / in Flash / in RAM, only when SPLC_REAL_ENABLE=1. */
    uint8_t  reserved[5];     /* Wire reg 13 low byte + regs 14..15: sender writes 0 */
#else
    uint8_t reserved[6];      /* Padding to make the struct size a multiple of 4 bytes */
#endif
} SPLC_RuleRecord;

/* The in-RAM record mirrors the 32-byte (16-register) wire record; Flash and the
 * CRC use the wire image, but rule_table_commit() memcpy()s the table. */
#ifndef __cplusplus
_Static_assert(sizeof(SPLC_RuleRecord) == 32u, "SPLC_RuleRecord must stay 32 bytes");
#endif

/*
 * SPLC_RuleRecord.flags (wire reg 13, high byte; Structs v2.0 / header
 * SPLC_RuleFlags_t). Bits 0 and 1 are orthogonal (App/firmware agreement,
 * handoff decision 55); bits 2..7 are stored and echoed back verbatim (so the
 * CRC over the wire image equals the App's) but not interpreted.
 *
 *   SPLC_RULE_FLAG_TRIGGER_FLOAT (bit 0, 0x01): the rule's TRIGGER side is
 *     float32. threshold_lo, threshold_hi and the current value of trigger_tag
 *     are IEEE-754 float32 bit patterns held in the int32 slots:
 *       - compare_op : float32 compare (EQ/NEQ with absolute tolerance
 *                      SPLC_RULE_FLOAT_EPSILON; GT/LT/GTE/LTE/BETWEEN exact)
 *   SPLC_RULE_FLAG_ACTION_FLOAT  (bit 1, 0x02): action_param is a float32 bit
 *     pattern. SET_TAG writes the raw action_param bits either way, so this bit
 *     changes no engine behaviour on its own; it is what makes SCALE_TAG float.
 *   SPLC_RULE_FLAG_FLOAT_DATA    (0x03): legacy alias = both bits.
 *
 *   - ACT_SCALE_TAG runs in float32 ONLY when both bits are set (0x03):
 *       action_tag = x * action_param_f + threshold_hi_f.
 *     With any other combination it runs the integer formula exactly as for an
 *     integer rule (decision 55; the App only emits 0x03 for a float Scale).
 *   - Mixed rule, e.g. "integer AI > 80 -> SET REAL = 2.5f" = flags 0x02: the
 *     compare stays integer (80 is compared raw) and SET writes the 2.5f bits.
 *   - Edge detection without compare, SET/TOGGLE/INC/ADD act on the raw 32-bit
 *     pattern exactly as for an integer rule (ADD_TAG with a float operand is
 *     NOT defined yet).
 * The engine reads ONLY these flags; it never consults the Tag DataType table
 * (plc_tagtype). With SPLC_REAL_ENABLE=0 the flags are neither carried nor
 * interpreted.
 */
#define SPLC_RULE_FLAG_TRIGGER_FLOAT 0x01u
#define SPLC_RULE_FLAG_ACTION_FLOAT  0x02u
#define SPLC_RULE_FLAG_FLOAT_DATA    (SPLC_RULE_FLAG_TRIGGER_FLOAT | SPLC_RULE_FLAG_ACTION_FLOAT)  /* 0x03, legacy alias */

/*
 * SPLC_RULE_FLAG_INT_TO_FLOAT (bit 2, 0x04; SPLC-SPEC-MATH-007 section 7). Meaningful ONLY on a
 * SCALE_TAG rule together with bits 0 and 1 (flags == 0x07) and only when SPLC_MATH_ENABLE=1: the
 * source tag then holds an int32 (e.g. a raw ADC count) that is converted with (float)(int32_t)
 * before y = x * gain + offset, instead of being read as float32 bits. Everywhere else bit 2 is
 * stored / echoed and ignored, as before. A device that does not declare capability bit 16
 * (0x0A08) treats 0x07 like 0x03: the App must not emit 0x07 to it.
 */
#define SPLC_RULE_FLAG_INT_TO_FLOAT  0x04u

/*
 * Verbatim carrying of the rest of wire reg 13 and regs 14..15 (SPLC-SPEC-MATH-007 section 2).
 * Only with SPLC_MATH_ENABLE=1 (which needs SPLC_REAL_ENABLE=1, so flags + reserved[5] exist):
 * reserved[0] = reg 13 low byte, reserved[1..2] = reg 14 (high byte first), reserved[3..4] =
 * reg 15. Every wire <-> record conversion goes through these, so the CRC over the wire image,
 * the Flash image and the read-back all agree with what the sender wrote.
 * With MATH=0 they do not exist and the callers keep the old "write 0 / ignore" code.
 */
#if SPLC_MATH_ENABLE
static inline uint16_t rule_wire_reg13(const SPLC_RuleRecord *r)
{ return (uint16_t)(((uint16_t)r->flags << 8) | r->reserved[0]); }
static inline uint16_t rule_wire_reg14(const SPLC_RuleRecord *r)
{ return (uint16_t)(((uint16_t)r->reserved[1] << 8) | r->reserved[2]); }
static inline uint16_t rule_wire_reg15(const SPLC_RuleRecord *r)
{ return (uint16_t)(((uint16_t)r->reserved[3] << 8) | r->reserved[4]); }
static inline void rule_set_wire_reg13(SPLC_RuleRecord *r, uint16_t w)
{ r->flags = (uint8_t)(w >> 8); r->reserved[0] = (uint8_t)(w & 0xFFU); }
static inline void rule_set_wire_reg14(SPLC_RuleRecord *r, uint16_t w)
{ r->reserved[1] = (uint8_t)(w >> 8); r->reserved[2] = (uint8_t)(w & 0xFFU); }
static inline void rule_set_wire_reg15(SPLC_RuleRecord *r, uint16_t w)
{ r->reserved[3] = (uint8_t)(w >> 8); r->reserved[4] = (uint8_t)(w & 0xFFU); }
#endif

/* Absolute tolerance used by float EQ / NEQ (same as the App's McuReferenceSimulator). */
#define SPLC_RULE_FLOAT_EPSILON     1e-5f

/*
 * RuleExecState drives the explicit state machine used by
 * rule_state_machine_step() (see plc_rule_state_machine.h). Every state is
 * re-evaluated from scratch every scan cycle EXCEPT RULE_STATE_DWELLING,
 * which is the only state that genuinely persists across multiple scan
 * cycles. See docs/architecture.md section 2.2b for the full rationale.
 */
typedef enum {
    RULE_STATE_IDLE = 0,
    RULE_STATE_TRIGGERED,
    RULE_STATE_COMPARED,
    RULE_STATE_DWELLING,    /* The only state that persists across scans */
    RULE_STATE_GUARD_CHECK,
    RULE_STATE_FIRE,
    RULE_STATE_BLOCKED,
} RuleExecState;

/* Sentinel meaning "dwell timer has not been armed yet". */
#define DWELL_NOT_STARTED 0xFFFFFFFFu

/*
 * Value Layer 4 passes as now_hhmm when there is no valid local time of day
 * (RTC not synced, or its timezone not yet written). SPLC_TRG_TIME_WINDOW
 * rules never fire while now_hhmm == RULE_HHMM_INVALID. Valid HHMM values
 * are 0..2359, so this can never collide with a real time.
 */
#define RULE_HHMM_INVALID 0xFFFFFFFFu

typedef struct {
    RuleExecState state;         /* Current position in the rule state machine */
    int32_t  prev_value;         /* Value of trigger_tag observed last scan, for edge detection */
    uint32_t dwell_start_tick;   /* Tick (ms) when DWELLING was entered; DWELL_NOT_STARTED if not dwelling */
    uint32_t last_fire_tick;     /* Tick (ms) of this rule's last FIRE, used by SPLC_TRG_INTERVAL */
} SPLC_RuleRuntime;

typedef struct {
    uint16_t rule_count;    //number of rules currently active in SPLC_RuleRecord g_rule_table[]
} SPLC_RuleTableInfo;

typedef struct {
    uint16_t rule_count;    //number of rules app write in staging
    uint16_t crc16;
} SPLC_RuleTransferInfo;

extern SPLC_RuleRecord          g_rule_table[MAX_RULES];
extern SPLC_RuleRuntime         g_rule_runtime[MAX_RULES];
extern SPLC_RuleTableInfo       g_rule_count;

/*
 * Load the rule table (g_rule_table[]) from Flash. Called exactly once at
 * boot, from plc_engine_init() (Layer 4). See the same Flash-access note
 * as tag_table_load_from_flash() in plc_tag.h: this function must not
 * include any Layer 0/1 header.
 */
void rule_table_load_from_flash(void);

/*
 * Evaluate every enabled rule in g_rule_table[] once. This is the single
 * entry point Layer 4 calls once per scan cycle (PLC_SCAN_INTERVAL_MS, see
 * plc_engine.h). Internally dispatches to check_trigger_edge(),
 * compare_ok(), dwell checking, guard checking, and execute_action() (all
 * internal to Layer 2).
 *
 * now_ms: current system tick in milliseconds, supplied by the caller
 *         (Layer 2 has no clock of its own). Only DIFFERENCES between two
 *         values are used (dwell: now - dwell_start; interval:
 *         now - last_fire), in unsigned arithmetic, so the counter may
 *         start at any value and wrap past UINT32_MAX (~49.7 days)
 *         without affecting behavior. Pass the same source every call and
 *         never let it go backwards.
 *
 *         Timing resolution equals the scan period: a dwell counts from
 *         the scan that DETECTED the edge, and completes on the first scan
 *         at or after its deadline -- up to one scan period late, never
 *         early.
 *
 * now_hhmm: local time of day as HHMM (0..2359, e.g. 08:30 -> 830) for
 *         SPLC_TRG_TIME_WINDOW, supplied by Layer 4 for the same reason as
 *         now_ms (Layer 2 has no RTC). Pass RULE_HHMM_INVALID when no valid
 *         time exists; Time Window rules then do not fire. Semantics
 *         (Structs doc V2.0 section 7), trigger_tag ignored, compare_op
 *         only matters as EQ + lo == hi:
 *           lo <= hi            window, true while lo <= hhmm <= hi
 *           lo >  hi            window across midnight, hhmm >= lo || hhmm <= hi
 *           compare_op == EQ and lo == hi   exact minute: fires ONCE on the
 *                               scan where hhmm becomes that minute
 *         Window rules fire on EVERY scan while the condition holds (level);
 *         use the exact-minute form for a one-shot action.
 */
void rule_scan(uint32_t now_ms, uint32_t now_hhmm);

/*
 * Reset every rule's runtime state (g_rule_runtime[]) to the same values a
 * freshly committed table has: RULE_STATE_IDLE, prev_value = 0, dwell not
 * started, last_fire_tick = 0. g_rule_table[] and g_rule_count are NOT
 * touched. Called by rule_table_commit(), and by Layer 4 when the Rule
 * Engine resumes after a diagnostic session (docs/handoff.md section 2.1).
 */
void rule_runtime_reset(void);

/*
 * Replace the active rule table with new data. Called from Layer 3
 * (plc_modbus_cfg.c) only after the incoming data has already passed its
 * own CRC-16/MODBUS check (see docs/architecture.md section 2.6.2 and the
 * v1.7 data contract; NOT CRC32).
 *
 * raw_data:    Buffer containing rule_count SPLC_RuleRecord structs,
 *              tightly packed (rule_count * sizeof(SPLC_RuleRecord) bytes)
 * rule_count:  Number of rules in raw_data
 * returns:     true on success, false on error (e.g. rule_count exceeds
 *              MAX_RULES)
 */
bool rule_table_commit(const uint8_t *raw_data, uint16_t rule_count);

/* Which tag field of a rule rule_table_find_bad_tag() complains about. */
typedef enum {
    RULE_TAG_FIELD_TRIGGER = 0,   /* trigger_tag */
    RULE_TAG_FIELD_ACTION,        /* action_tag */
    RULE_TAG_FIELD_GUARD          /* guard_tag (index bits; the NEGATE bit is ignored) */
} SPLC_RuleTagField;

/*
 * Find the first ENABLED rule that names a tag index outside 0 .. MAX_TAGS-1 in a field
 * the engine really uses. Such a rule would not crash anything: tag_read() returns 0 and
 * tag_write() does nothing for an out-of-range index, so the rule just never does what
 * its author meant, with no error anywhere. Layer 3 calls this before rule_table_commit()
 * and refuses the COMMIT (plc_modbus_cfg.c).
 *
 * A field is checked only when the engine reads or writes it:
 *   trigger_tag : edge triggers (ON_CHANGE / ON_RISE / ON_FALL); any other trigger except
 *                 TIME_WINDOW when compare_op != NONE; always for ADD_TAG / SCALE_TAG (it is
 *                 their source). Ignored for TIME_WINDOW and for INTERVAL without a compare.
 *   action_tag  : SET_TAG, TOGGLE_TAG, INC_COUNTER, SEND_ALARM, ADD_TAG, SCALE_TAG.
 *                 Ignored for WRITE_REMOTE and LOG_EVENT (not implemented yet) and for an
 *                 unknown action type.
 *   guard_tag   : always, except the "no guard" sentinel (index bits == GUARD_TAG_NONE).
 * A DISABLED rule is skipped (the engine never evaluates it).
 * Only the index range is checked, not the tag kind: a slot of kind TAG_NONE inside the
 * range is accepted.
 *
 * table, rule_count : the table to check (rule_count is clamped to MAX_RULES)
 * field, tag_value  : optional (may be NULL); set only when a bad rule is found:
 *                     the field and the offending raw value (for guard_tag the full 16 bits)
 * returns           : index of the first bad rule, or -1 when every rule is fine
 *                     (also -1 for table == NULL)
 */
int rule_table_find_bad_tag(const SPLC_RuleRecord *table, uint16_t rule_count,
                            SPLC_RuleTagField *field, uint16_t *tag_value);

#if SPLC_MATH_ENABLE
/* Why rule_table_find_bad_math() refused a rule. */
typedef enum {
    RULE_MATH_BAD_OPERATOR = 0,    /* compare_op > 4 */
    RULE_MATH_BAD_FLAGS,           /* flags is neither 0x00 (int32) nor 0x03 (float32) */
    RULE_MATH_CONST_ZERO_DIVISOR   /* DIV / MOD whose constant Op2 (aux) is 0 / +0.0 / -0.0 */
} SPLC_RuleMathReason;

/*
 * COMMIT check of the MATH rule block (SPLC-SPEC-MATH-007 section 5, items 1, 2 and 5). Only
 * ENABLED rules with action_type == SPLC_ACT_MATH_OP are looked at. The tag-index items (3, 4)
 * are covered by rule_table_find_bad_tag(): MATH counts there as using trigger_tag (Op1),
 * guard_tag (Op2, unless it is the constant sentinel) and action_tag (OUT, which must be a real
 * tag, so a System Tag 1000..1007 is refused). Not checked, on purpose: Reg 6..7 (ignored),
 * action_tag equal to an operand, the type of the OUT tag against `flags`.
 *
 * reason : optional; set only when a bad rule is found
 * returns: index of the first bad rule, or -1 (also -1 for table == NULL)
 */
int rule_table_find_bad_math(const SPLC_RuleRecord *table, uint16_t rule_count,
                             SPLC_RuleMathReason *reason);
#endif

#ifdef __cplusplus
}
#endif

#endif /* PLC_RULE_H */