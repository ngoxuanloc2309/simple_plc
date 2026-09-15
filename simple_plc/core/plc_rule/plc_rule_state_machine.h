#ifndef PLC_RULE_STATE_MACHINE_H
#define PLC_RULE_STATE_MACHINE_H

/*
 * plc_rule_state_machine.h - Layer 2 (PLC Core), internal to plc_rule.c
 *
 * Declares the single-rule state machine step function used by rule_scan()
 * to evaluate one SPLC_RuleRecord against its SPLC_RuleRuntime once per
 * scan cycle.
 *
 * RuleExecState, DWELL_NOT_STARTED, and SPLC_RuleRuntime itself are defined
 * in plc_rule.h (not here) because SPLC_RuleRuntime is part of the Layer 2
 * public surface (g_rule_runtime[] is declared extern there). This header
 * only adds the function declaration that operates on those types.
 *
 * This header is not part of the Layer 2 public API; only plc_rule.c is
 * expected to include it.
 *
 * See docs/architecture.md, section 2.2b, "State Machine #1".
 */

#include <stdbool.h>
#include <stdint.h>

#include "plc_rule.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Advance one rule's state machine by exactly one scan cycle.
 *
 * States IDLE -> TRIGGERED -> COMPARED -> GUARD_CHECK -> FIRE are all
 * resolved within the same call via deliberate fallthrough, since Trigger/
 * Compare/Guard are instantaneous checks with no cross-scan memory.
 * RULE_STATE_DWELLING is the only state that genuinely persists: if
 * for_ms > 0, the function returns false after arming the dwell timer and
 * only continues toward GUARD_CHECK/FIRE once enough scan cycles have
 * elapsed with the trigger condition still holding.
 *
 * rule:   The rule definition to evaluate. Must not be NULL.
 * rt:     This rule's runtime state, persisted across calls in
 *         g_rule_runtime[]. Must not be NULL.
 * now_ms: Current system tick, in milliseconds.
 *
 * returns: true if the rule fired (reached RULE_STATE_FIRE) this call,
 *          false otherwise (blocked, dwelling, or disabled).
 */
bool rule_state_machine_step(SPLC_RuleRecord *rule, SPLC_RuleRuntime *rt, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* PLC_RULE_STATE_MACHINE_H */