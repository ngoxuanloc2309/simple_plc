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
 * FIX (review vs. spec section 4.1/6.5): the previous signature folded
 * TRG_INTERVAL's period and TRG_TIME_WINDOW's HHMM bounds into this
 * function and additionally reused Rule.for_ms as that period. That
 * collided with for_ms's other, spec-defined meaning (dwell/debounce for
 * ON_CHANGE/ON_RISE/ON_FALL, applied separately in rule_scan()/dwell_ok()):
 * a single field was being interpreted two different ways in the same
 * call, and TRG_INTERVAL rules got an extra, incorrect dwell check applied
 * on top by rule_scan(). This function is now edge-detection only, exactly
 * mirroring the "Trigger (change edge?)" step in architecture.md section
 * 6.5 / spec section 6.5. Timing-based triggers (TRG_TIME_WINDOW,
 * TRG_INTERVAL) are handled by trigger_timing_ok() below instead, which
 * consumes for_ms/threshold_lo/threshold_hi with the meaning the spec
 * actually assigns them per trigger_type (spec section 4.2).
 *
 * Evaluate whether an edge-based trigger condition is currently met.
 * Only meaningful for TRG_ON_CHANGE / TRG_ON_RISE / TRG_ON_FALL.
 * TRG_TIME_WINDOW and TRG_INTERVAL are level/timing triggers, not edges;
 * for those two this function returns true unconditionally so rule_scan()
 * can move on to trigger_timing_ok(), which is the real gate for them.
 *
 * type:      Trigger type to evaluate
 * prev:      Value observed on the previous scan cycle
 * current:   Value observed on the current scan cycle
 *
 * returns: true if the trigger condition is currently satisfied
 */
bool check_trigger_edge(TriggerType type,
                         int32_t prev,
                         int32_t current);

/*
 * Evaluate TRG_TIME_WINDOW / TRG_INTERVAL, the two trigger types that are
 * not edge-based and instead depend on time. Per spec section 4.2:
 *   - TRG_TIME_WINDOW: threshold_lo/threshold_hi are HHMM marks (e.g. 700,
 *     1600), trigger_tag is ignored. now_hhmm must be wall-clock
 *     time-of-day in HHMM form, NOT a millisecond tick count -- the
 *     previous implementation compared an HHMM threshold against a raw ms
 *     tick, which can never match except by coincidence. Producing
 *     now_hhmm requires an RTC/calendar source Layer 2 does not have;
 *     see the TODO at the call site in plc_rule.c.
 *   - TRG_INTERVAL: for_ms is the fire period; trigger_tag is ignored.
 *     Fires when now_ms - last_fire_ms >= for_ms.
 * For TRG_ON_CHANGE/ON_RISE/ON_FALL this function is not applicable and
 * should not be called; rule_scan() only calls it for the two timing
 * trigger types.
 *
 * type:         Trigger type to evaluate (only TRG_TIME_WINDOW/TRG_INTERVAL)
 * now_ms:       Current system tick, in milliseconds (used by TRG_INTERVAL)
 * now_hhmm:     Current time-of-day as HHMM (used by TRG_TIME_WINDOW)
 * threshold_lo: TRG_TIME_WINDOW window start (HHMM); unused for TRG_INTERVAL
 * threshold_hi: TRG_TIME_WINDOW window end (HHMM); unused for TRG_INTERVAL
 * for_ms:       TRG_INTERVAL period in ms; unused for TRG_TIME_WINDOW
 * last_fire_ms: Tick of this rule's last fire (TRG_INTERVAL only)
 *
 * returns: true if the timing condition is currently satisfied
 */
bool trigger_timing_ok(TriggerType type,
                        uint32_t now_ms,
                        uint32_t now_hhmm,
                        int32_t threshold_lo,
                        int32_t threshold_hi,
                        uint32_t for_ms,
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