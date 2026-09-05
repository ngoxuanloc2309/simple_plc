/*
 * plc_rule_eval.c - Layer 2 (PLC Core), internal to plc_rule.c
 *
 * Implementation of trigger edge detection and comparison evaluation.
 * No dependency on plc_tag.h or any global state; every function here
 * operates purely on its arguments.
 */

#include "plc_rule_eval.h"

/*
 * FIX: see the comment in plc_rule_eval.h. This is now edge-detection
 * only for the three edge trigger types. TRG_TIME_WINDOW and TRG_INTERVAL
 * are level/timing triggers, not edges -- they have no "prev vs current"
 * concept -- so they return true here and defer to trigger_timing_ok() as
 * the real gate. Returning false for them (the old default: case) would
 * have made rule_scan() reject every TRG_TIME_WINDOW/TRG_INTERVAL rule
 * outright, and both the spec examples (4.4 R0a/R0b use TRG_TIME_WINDOW;
 * 4.5 R2 uses TRG_INTERVAL) fire in practice, so that default was wrong.
 */
bool check_trigger_edge(TriggerType type,
                         int32_t prev,
                         int32_t current)
{
    switch (type) {
        case TRG_ON_CHANGE:
            return current != prev;

        case TRG_ON_RISE:
            return prev == 0 && current != 0;

        case TRG_ON_FALL:
            return prev != 0 && current == 0;

        case TRG_TIME_WINDOW:
        case TRG_INTERVAL:
            /* Not edge-based; gated by trigger_timing_ok() instead. */
            return true;

        default:
            return false;
    }
}

/*
 * FIX: new function, split out of the old check_trigger_edge() so that
 * TRG_TIME_WINDOW's HHMM window and TRG_INTERVAL's period no longer share
 * a parameter (and Rule.for_ms) with edge-trigger dwell handling in
 * plc_rule.c. See plc_rule_eval.h for the full rationale.
 */
bool trigger_timing_ok(TriggerType type,
                        uint32_t now_ms,
                        uint32_t now_hhmm,
                        int32_t threshold_lo,
                        int32_t threshold_hi,
                        uint32_t for_ms,
                        uint32_t last_fire_ms)
{
    switch (type) {
        case TRG_TIME_WINDOW:
            if (threshold_lo <= threshold_hi) {
                return (int32_t)now_hhmm >= threshold_lo && (int32_t)now_hhmm <= threshold_hi;
            }
            /* Window wraps past midnight (e.g. 2300-0100). */
            return (int32_t)now_hhmm >= threshold_lo || (int32_t)now_hhmm <= threshold_hi;

        case TRG_INTERVAL:
            return (now_ms - last_fire_ms) >= for_ms;

        default:
            /* Not a timing trigger; caller should not reach here. */
            return true;
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