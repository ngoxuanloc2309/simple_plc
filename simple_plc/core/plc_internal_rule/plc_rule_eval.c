/*
 * plc_rule_eval.c - Layer 2 (PLC Core), internal to plc_rule.c
 *
 * Implementation of trigger edge detection and comparison evaluation.
 * No dependency on plc_tag.h or any global state; every function here
 * operates purely on its arguments.
 */

#include "plc_rule_eval.h"

#include <string.h>

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
bool check_trigger_edge(SPLC_TriggerType type,
                         int32_t prev,
                         int32_t current)
{
    switch (type) {
        case SPLC_TRG_ON_CHANGE:
            return current != prev;

        case SPLC_TRG_ON_RISE:
            return prev == 0 && current != 0;

        case SPLC_TRG_ON_FALL:
            return prev != 0 && current == 0;

        case SPLC_TRG_TIME_WINDOW:
        case SPLC_TRG_INTERVAL:
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
bool trigger_timing_ok(SPLC_TriggerType type,
                        uint32_t now_ms,
                        uint32_t now_hhmm,
                        int32_t threshold_lo,
                        int32_t threshold_hi,
                        uint32_t for_ms,
                        uint32_t last_fire_ms)
{
    switch (type) {
        case SPLC_TRG_TIME_WINDOW:
            if (now_hhmm == RULE_HHMM_INVALID) {
                return false;   /* no valid local time: never in the window */
            }
            if (threshold_lo <= threshold_hi) {
                return (int32_t)now_hhmm >= threshold_lo && (int32_t)now_hhmm <= threshold_hi;
            }
            /* Window wraps past midnight (e.g. 2300-0100). */
            return (int32_t)now_hhmm >= threshold_lo || (int32_t)now_hhmm <= threshold_hi;

        case SPLC_TRG_INTERVAL:
            return (now_ms - last_fire_ms) >= for_ms;

        default:
            /* Not a timing trigger; caller should not reach here. */
            return true;
    }
}

bool compare_ok(SPLC_CompareOp op, int32_t current, int32_t lo, int32_t hi)
{
    switch (op) {
        case SPLC_OP_NONE:
            return true;

        case SPLC_OP_EQ:
            return current == lo;

        case SPLC_OP_NEQ:
            return current != lo;

        case SPLC_OP_GT:
            return current > lo;

        case SPLC_OP_LT:
            return current < lo;

        case SPLC_OP_GTE:
            return current >= lo;

        case SPLC_OP_LTE:
            return current <= lo;

        case SPLC_OP_BETWEEN:
            return current >= lo && current <= hi;

        default:
            return false;
    }
}

#if SPLC_REAL_ENABLE
float splc_bits_to_f32(int32_t bits)
{
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

int32_t splc_f32_to_bits(float value)
{
    int32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

/* |x| without libm; NaN stays NaN (every comparison below is then false). */
static float abs_f32(float x) { return (x < 0.0f) ? -x : x; }

bool compare_ok_f(SPLC_CompareOp op, int32_t current, int32_t lo, int32_t hi)
{
    const float v = splc_bits_to_f32(current);
    const float l = splc_bits_to_f32(lo);

    switch (op) {
        case SPLC_OP_NONE:    return true;
        case SPLC_OP_EQ:      return abs_f32(v - l) <= SPLC_RULE_FLOAT_EPSILON;
        case SPLC_OP_NEQ:     return abs_f32(v - l) >  SPLC_RULE_FLOAT_EPSILON;
        case SPLC_OP_GT:      return v >  l;
        case SPLC_OP_LT:      return v <  l;
        case SPLC_OP_GTE:     return v >= l;
        case SPLC_OP_LTE:     return v <= l;
        case SPLC_OP_BETWEEN: return v >= l && v <= splc_bits_to_f32(hi);
        default:              return false;
    }
}
#endif /* SPLC_REAL_ENABLE */