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

/* Maximum number of rules supported system-wide. */
#define MAX_RULES 100

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
 * 15 bits wide, so it can never equal a real tag index: MAX_TAGS is 128
 * (indices 0..127), far below 32767. Every valid tag index (0..127) is
 * therefore always distinguishable from "no guard".
 *
 * rule_table_commit() does not currently reject a guard_tag whose index
 * bits fall in 128..32766 (a value that is neither a valid tag nor this
 * sentinel) -- see the TODO in plc_rule.c's rule_state_machine_step().
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
    SPLC_ACT_SCALE_TAG,       /* action_tag = tag_read(trigger_tag) * action_param / 1000 + threshold_hi */
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
    uint8_t reserved[6];      /* Padding to make the struct size a multiple of 4 bytes */
} SPLC_RuleRecord;

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
 */
void rule_scan(uint32_t now_ms);

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

#ifdef __cplusplus
}
#endif

#endif /* PLC_RULE_H */