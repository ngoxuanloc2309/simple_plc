/*
 * plc_rule.c - Layer 2 (PLC Core)
 *
 * Implementation of the Rule Engine's main scan loop and rule table
 * management. Depends on plc_tag.h and the internal evaluation/action
 * helpers within Layer 2; must not include anything from Layer 0/1.
 */

#include "plc_rule.h"

#include <string.h>

#include "plc_rule_action.h"
#include "plc_rule_eval.h"
#include "plc_tag.h"

Rule        g_rule_table[MAX_RULES];
RuleRuntime g_rule_runtime[MAX_RULES];
int         g_rule_count = 0;

/*
 * TODO: system tick source is not wired up yet. Layer 4 is expected to
 * provide the current tick count (see docs/architecture.md, plc_engine.c).
 * Until that is in place, this returns 0, which disables TRG_INTERVAL and
 * TRG_TIME_WINDOW behavior without crashing.
 */
static uint32_t get_system_tick_ms(void)
{
    return 0;
}

static bool guard_ok(uint16_t guard_tag)
{
    uint16_t index  = guard_tag & GUARD_TAG_INDEX_MASK;
    bool     negate = (guard_tag & GUARD_TAG_NEGATE_BIT) != 0;

    if (index == 0 && guard_tag == 0) {
        /* No guard configured; always allow. */
        return true;
    }

    bool guard_value = tag_read(index) != 0;
    return negate ? !guard_value : guard_value;
}

static bool dwell_ok(RuleRuntime *runtime, bool condition_met, uint32_t for_ms, uint32_t now_ms)
{
    if (for_ms == 0) {
        return condition_met;
    }

    if (!condition_met) {
        runtime->condition_since_tick = 0;
        return false;
    }

    if (runtime->condition_since_tick == 0) {
        runtime->condition_since_tick = now_ms;
        return false;
    }

    return (now_ms - runtime->condition_since_tick) >= for_ms;
}

void rule_table_load_from_flash(void)
{
    /*
     * TODO: same pattern as tag_table_load_from_flash(). This function
     * must not call sx_flash_read() directly; the actual Flash-backed
     * buffer is expected to be supplied by Layer 3/4. For now this only
     * guarantees a defined, empty starting state.
     */
    memset(g_rule_table, 0, sizeof(g_rule_table));
    memset(g_rule_runtime, 0, sizeof(g_rule_runtime));
    g_rule_count = 0;
}

void rule_scan(void)
{
    uint32_t now_ms = get_system_tick_ms();

    for (int i = 0; i < g_rule_count; i++) {
        Rule        *rule    = &g_rule_table[i];
        RuleRuntime *runtime = &g_rule_runtime[i];

        if (!rule->enabled) {
            continue;
        }

        int32_t current = tag_read(rule->trigger_tag);

        bool trigger_met = check_trigger_edge((TriggerType)rule->trigger_type,
                                                runtime->prev_value,
                                                current,
                                                now_ms,
                                                (uint32_t)rule->threshold_lo,
                                                (uint32_t)rule->threshold_hi,
                                                rule->for_ms,
                                                runtime->last_fire_tick);
        runtime->prev_value = current;

        if (!trigger_met) {
            continue;
        }

        if (!compare_ok((CompareOp)rule->compare_op, current, rule->threshold_lo, rule->threshold_hi)) {
            continue;
        }

        if (!dwell_ok(runtime, true, rule->for_ms, now_ms)) {
            continue;
        }

        if (!guard_ok(rule->guard_tag)) {
            continue;
        }

        execute_action(rule);
        runtime->last_fire_tick = now_ms;
    }
}

int rule_table_commit(const uint8_t *raw_data, int rule_count)
{
    if (raw_data == NULL) {
        return -1;
    }

    if (rule_count < 0 || rule_count > MAX_RULES) {
        return -2;
    }

    memcpy(g_rule_table, raw_data, (size_t)rule_count * sizeof(Rule));
    memset(g_rule_runtime, 0, sizeof(g_rule_runtime));
    g_rule_count = rule_count;

    return 0;
}