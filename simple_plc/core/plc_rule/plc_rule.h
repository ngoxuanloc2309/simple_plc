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
    uint16_t guard_tag;       /* Packed guard tag index + NEGATE bit */
    uint8_t  enabled;
    uint8_t  trigger_type;    /* One of TriggerType */
    uint8_t  compare_op;      /* One of CompareOp */
    uint8_t  action_type;     /* One of ActionType */
    uint8_t reserved[6];      /* Padding to make the struct size a multiple of 4 bytes */
} SPLC_RuleRecord;

typedef struct {
    int32_t  prev_value;
    uint32_t condition_since_tick;
    uint32_t last_fire_tick;
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
 * entry point Layer 4 calls once per scan cycle (every 10 ms). Internally
 * dispatches to check_trigger_edge(), compare_ok(), dwell checking, guard
 * checking, and execute_action() (all internal to Layer 2).
 */
void rule_scan(void);

/*
 * Replace the active rule table with new data. Called from Layer 3
 * (plc_modbus_cfg.c) only after the incoming data has already passed its
 * own CRC32 check.
 *
 * raw_data:    Buffer containing rule_count Rule structs, tightly packed
 * rule_count:  Number of rules in raw_data
 * returns:     0 on success, negative value on error (e.g. rule_count
 *              exceeds MAX_RULES)
 */
int rule_table_commit(const uint8_t *raw_data, int rule_count);

#ifdef __cplusplus
}
#endif

#endif /* PLC_RULE_H */