/*
 * plc_rule_eval.c - Layer 2 (PLC Core), internal to plc_rule.c
 *
 * Implementation of trigger edge detection and comparison evaluation.
 * No dependency on plc_tag.h or any global state; every function here
 * operates purely on its arguments.
 */

#include "plc_rule_eval.h"

bool check_trigger_edge(TriggerType type,
                         int32_t prev,
                         int32_t current,
                         uint32_t now_ms,
                         uint32_t window_lo,
                         uint32_t window_hi,
                         uint32_t interval_ms,
                         uint32_t last_fire_ms)
{
    switch (type) {
        case TRG_ON_CHANGE:
            return current != prev;

        case TRG_ON_RISE:
            return prev == 0 && current != 0;

        case TRG_ON_FALL:
            return prev != 0 && current == 0;

        case TRG_TIME_WINDOW:
            if (window_lo <= window_hi) {
                return now_ms >= window_lo && now_ms <= window_hi;
            }
            /* Window wraps past the reference boundary (e.g. 23:00-01:00). */
            return now_ms >= window_lo || now_ms <= window_hi;

        case TRG_INTERVAL:
            return (now_ms - last_fire_ms) >= interval_ms;

        default:
            return false;
    }
}

bool compare_ok(CompareOp op, int32_t current, int32_t lo, int32_t hi)
{
    switch (op) {
        case OP_NONE:
            return true;

        case OP_EQ:
            return current == lo;

        case OP_NEQ:
            return current != lo;

        case OP_GT:
            return current > lo;

        case OP_LT:
            return current < lo;

        case OP_GTE:
            return current >= lo;

        case OP_LTE:
            return current <= lo;

        case OP_BETWEEN:
            return current >= lo && current <= hi;

        default:
            return false;
    }
}