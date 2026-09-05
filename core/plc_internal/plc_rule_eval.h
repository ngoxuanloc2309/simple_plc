#ifndef PLC_RULE_EVAL_H
#define PLC_RULE_EVAL_H

/*
 * plc_rule_eval.h - Layer 2 (PLC Core), internal to plc_rule.c
 *
 * Pure evaluation helpers for the Rule Engine: trigger edge detection and
 * value comparison. These functions are stateless with respect to the
 * rule table and depend only on the arguments passed in, which makes them
 * straightforward to unit test in isolation.
 *
 * This header is not part of the Layer 2 public API; only plc_rule.c is
 * expected to include it.
 */

#include <stdbool.h>
#include <stdint.h>

#include "plc_rule.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Evaluate whether a trigger condition is currently met.
 *
 * type:      Trigger type to evaluate
 * prev:      Value observed on the previous scan cycle
 * current:   Value observed on the current scan cycle
 * now_ms:    Current system tick, in milliseconds
 * window_lo: For TRG_TIME_WINDOW, lower bound of the active window (ms
 *            since some epoch/day reference); unused otherwise
 * window_hi: For TRG_TIME_WINDOW, upper bound of the active window;
 *            unused otherwise
 * interval_ms: For TRG_INTERVAL, the period between fires; unused otherwise
 * last_fire_ms: For TRG_INTERVAL, the tick of the last fire; unused otherwise
 *
 * returns: true if the trigger condition is currently satisfied
 */
bool check_trigger_edge(TriggerType type,
                         int32_t prev,
                         int32_t current,
                         uint32_t now_ms,
                         uint32_t window_lo,
                         uint32_t window_hi,
                         uint32_t interval_ms,
                         uint32_t last_fire_ms);

/*
 * Evaluate a comparison operator against a current value and one or two
 * thresholds.
 *
 * op:      Comparison operator to apply
 * current: Value to test
 * lo:      Lower threshold (used by OP_GT/OP_LT/OP_GTE/OP_LTE/OP_BETWEEN)
 * hi:      Upper threshold (only used by OP_BETWEEN)
 *
 * returns: true if the comparison holds
 */
bool compare_ok(CompareOp op, int32_t current, int32_t lo, int32_t hi);

#ifdef __cplusplus
}
#endif

#endif /* PLC_RULE_EVAL_H */