#ifndef PLC_RULE_ACTION_H
#define PLC_RULE_ACTION_H

/*
 * plc_rule_action.h - Layer 2 (PLC Core), internal to plc_rule.c
 *
 * Action dispatch for the Rule Engine. This header is not part of the
 * Layer 2 public API; only plc_rule.c is expected to include it.
 */

#include "plc_rule.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Execute the action configured on a rule that has just fired. Dispatches
 * internally based on rule->action_type. Only ever calls tag_read()/
 * tag_write() from Layer 2; never touches hardware directly.
 *
 * rule: The rule whose action should run. Must not be NULL.
 */
void execute_action(Rule *rule);

#ifdef __cplusplus
}
#endif

#endif /* PLC_RULE_ACTION_H */