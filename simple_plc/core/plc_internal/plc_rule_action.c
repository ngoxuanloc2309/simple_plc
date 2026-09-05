/*
 * plc_rule_action.c - Layer 2 (PLC Core), internal to plc_rule.c
 *
 * Implementation of execute_action(). Depends on plc_tag.h for tag_read()/
 * tag_write(); must not include anything from Layer 0/1.
 */

#include "plc_rule_action.h"

#include "plc_tag.h"

/*
 * TODO: ACT_WRITE_REMOTE, ACT_LOG_EVENT, and ACT_SEND_ALARM are only
 * loosely specified in the base spec ("flag a pending remote write",
 * "append to an internal RAM log", "set an alarm code"). The data
 * structures backing the pending-write queue and the RAM log buffer have
 * not been designed yet. The stubs below implement the minimal behavior
 * that is unambiguous from the spec (setting a tag value) and leave the
 * rest for a follow-up design pass.
 */

static void action_set_tag(const Rule *rule)
{
    tag_write(rule->action_tag, rule->action_param);
}

static void action_toggle_tag(const Rule *rule)
{
    int32_t current = tag_read(rule->action_tag);
    tag_write(rule->action_tag, current == 0 ? 1 : 0);
}

static void action_inc_counter(const Rule *rule)
{
    int32_t current = tag_read(rule->action_tag);
    tag_write(rule->action_tag, current + rule->action_param);
}

static void action_write_remote(const Rule *rule)
{
    /*
     * TODO: pending-remote-write queue is not designed yet (see the note
     * above). For now this only records the intent locally so remote
     * write requests are not silently lost once the queue exists.
     */
    (void)rule;
}

static void action_log_event(const Rule *rule)
{
    /* TODO: RAM event log buffer is not designed yet (see note above). */
    (void)rule;
}

static void action_send_alarm(const Rule *rule)
{
    /*
     * Minimal, unambiguous interpretation of the spec: store the alarm
     * code into the tag designated by action_tag.
     */
    tag_write(rule->action_tag, rule->action_param);
}

static void action_add_tag(const Rule *rule)
{
    int32_t current  = tag_read(rule->action_tag);
    int32_t trigger  = tag_read(rule->trigger_tag);
    tag_write(rule->action_tag, current + trigger);
}

static void action_scale_tag(const Rule *rule)
{
    int32_t trigger_value = tag_read(rule->trigger_tag);
    int32_t scaled = (trigger_value * rule->action_param) / 1000 + rule->threshold_hi;
    tag_write(rule->action_tag, scaled);
}

void execute_action(Rule *rule)
{
    switch ((ActionType)rule->action_type) {
        case ACT_SET_TAG:
            action_set_tag(rule);
            break;
        case ACT_TOGGLE_TAG:
            action_toggle_tag(rule);
            break;
        case ACT_INC_COUNTER:
            action_inc_counter(rule);
            break;
        case ACT_WRITE_REMOTE:
            action_write_remote(rule);
            break;
        case ACT_LOG_EVENT:
            action_log_event(rule);
            break;
        case ACT_SEND_ALARM:
            action_send_alarm(rule);
            break;
        case ACT_ADD_TAG:
            action_add_tag(rule);
            break;
        case ACT_SCALE_TAG:
            action_scale_tag(rule);
            break;
        default:
            break;
    }
}