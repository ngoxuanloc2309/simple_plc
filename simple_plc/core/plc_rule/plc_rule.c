#include <string.h>

#include "plc_rule_state_machine.h"
#include "plc_rule_eval.h"   /* check_trigger_edge(), compare_ok(), trigger_timing_ok() */
#include "plc_rule_action.h" /* execute_action() */
#include "plc_tag.h"
#include "logger.h"

static const char *TAG = "PLC_RULE";

SPLC_RuleRecord   g_rule_table[MAX_RULES];
SPLC_RuleRuntime  g_rule_runtime[MAX_RULES];
SPLC_RuleTableInfo g_rule_count;

void rule_table_load_from_flash(void)
{
    /*
     * This function only guarantees a defined, empty starting state
     * (rule_count = 0, every runtime slot IDLE) so g_rule_table[]/
     * g_rule_runtime[] are never left uninitialized. It does NOT read
     * Flash itself -- Layer 2 must stay buildable/testable on a plain PC,
     * so it must never call sx_flash_*() (Layer 0/1) directly.
     *
     * Actually loading a previously-committed rule table from Flash is
     * services/plc_rule_flash/plc_rule_flash.c's plc_rule_flash_load()
     * (Layer 3): it reads Flash sectors A/B, then calls
     * rule_table_commit() (below) with whatever valid data it finds --
     * see docs/handoff.md section 1 and plc_rule_flash.h for the full
     * two-sector recovery mechanism. plc_engine_init() (Layer 4) calls
     * this function FIRST, then plc_rule_flash_load() -- so a device with
     * no valid Flash record yet (first boot, or both A/B corrupted)
     * simply keeps the empty table this function establishes, which is
     * an expected, non-error state rather than something
     * plc_rule_flash_load() needs to special-case.
     */
    memset(g_rule_table, 0, sizeof(g_rule_table));
    for (int i = 0; i < MAX_RULES; i++) {
        g_rule_runtime[i].state             = RULE_STATE_IDLE;
        g_rule_runtime[i].prev_value        = 0;
        g_rule_runtime[i].dwell_start_tick  = DWELL_NOT_STARTED;
        g_rule_runtime[i].last_fire_tick    = 0;
    }
    g_rule_count.rule_count = 0;
}

void rule_scan(uint32_t now_ms)
{
    /*
     * now_ms is supplied by the caller (Layer 4) instead of being read
     * here: Layer 2 must not include a Layer 0/1 timer header. Taking it as
     * a PARAMETER (rather than a static the caller must remember to update
     * first) means a caller that forgets the time cannot compile. One value
     * is shared by every rule in this pass, so all rules in one scan cycle
     * see the same "now".
     */
    for (uint16_t i = 0; i < g_rule_count.rule_count && i < MAX_RULES; i++) {
        rule_state_machine_step(&g_rule_table[i], &g_rule_runtime[i], now_ms);
    }
}

bool rule_table_commit(const uint8_t *raw_data, uint16_t rule_count)
{
    /*
     * TODO: per docs/architecture.md section 2.6.2 / the v1.7 data
     * contract, the caller (plc_modbus_cfg.c, Layer 3 -- currently empty)
     * is responsible for verifying CRC-16/MODBUS over
     * rule_count * sizeof(SPLC_RuleRecord) bytes BEFORE calling this
     * function. This function only performs the atomic swap into
     * g_rule_table[]; it does not re-verify CRC itself.
     */
    if (raw_data == NULL || rule_count > MAX_RULES) {
        log_warn(TAG, "rule_table_commit rejected: raw_data=%p rule_count=%u (MAX_RULES=%u)",
                 (const void *)raw_data, rule_count, (unsigned)MAX_RULES);
        return false;
    }

    memcpy(g_rule_table, raw_data, (size_t)rule_count * sizeof(SPLC_RuleRecord));
    if (rule_count < MAX_RULES) {
        memset(&g_rule_table[rule_count], 0,
               (size_t)(MAX_RULES - rule_count) * sizeof(SPLC_RuleRecord));
    }

    for (int i = 0; i < MAX_RULES; i++) {
        g_rule_runtime[i].state             = RULE_STATE_IDLE;
        g_rule_runtime[i].prev_value        = 0;
        g_rule_runtime[i].dwell_start_tick  = DWELL_NOT_STARTED;
        g_rule_runtime[i].last_fire_tick    = 0;
    }
    g_rule_count.rule_count = rule_count;
    log_info(TAG, "rule table committed: %u rule(s) active, runtime state reset", rule_count);
    return true;
}

/*
 * State machine re-evaluated FROM SCRATCH every scan cycle for the
 * Trigger/Compare/Guard steps (they have no concept of "persisting across
 * scan cycles" -- Trigger is an instantaneous event, Compare/Guard are
 * point-in-time comparisons), BUT DWELLING is the ONE state that genuinely
 * "holds" across multiple calls to this function, exactly as
 * SPLC_RuleRuntime keeps rt->state = RULE_STATE_DWELLING between calls.
 *
 * This preserves the intended semantics: dwell must be measured against
 * the current LEVEL (via compare_ok()), not against a one-shot edge-detect
 * result (see the historical note in the DWELLING case below) -- otherwise
 * dwell would never complete for edge-type triggers.
 */
bool rule_state_machine_step(SPLC_RuleRecord *rule, SPLC_RuleRuntime *rt, uint32_t now_ms)
{
    if (!rule->enabled) {
        rt->state = RULE_STATE_IDLE;
        return false;
    }

    SPLC_TriggerType trigger_type = (SPLC_TriggerType)rule->trigger_type;
    bool              is_edge_type = (trigger_type == SPLC_TRG_ON_RISE  ||
                                       trigger_type == SPLC_TRG_ON_FALL  ||
                                       trigger_type == SPLC_TRG_ON_CHANGE);
    int32_t           current       = tag_read(rule->trigger_tag);

    switch (rt->state) {

    case RULE_STATE_IDLE:
    case RULE_STATE_BLOCKED: {
        /* Re-evaluated from scratch every scan cycle when not mid-dwell. */
        bool trigger_met;
        if (is_edge_type) {
            trigger_met = check_trigger_edge(trigger_type, rt->prev_value, current);
        } else {
            /* SPLC_TRG_TIME_WINDOW / SPLC_TRG_INTERVAL.
             * TODO: now_hhmm is hardcoded to 0 until an RTC/calendar
             * source is available (see docs/architecture.md section 10).
             * This means SPLC_TRG_TIME_WINDOW cannot function correctly
             * yet; SPLC_TRG_INTERVAL is unaffected since it only uses
             * now_ms. */
            trigger_met = trigger_timing_ok(trigger_type, now_ms, 0,
                                             rule->threshold_lo, rule->threshold_hi,
                                             rule->for_ms, rt->last_fire_tick);
        }
        rt->prev_value = current;

        if (!trigger_met) {
            rt->state = RULE_STATE_BLOCKED;
            return false;
        }
        rt->state = RULE_STATE_TRIGGERED;
    }
    /* FALLTHROUGH */

    case RULE_STATE_TRIGGERED: {
        if (rule->compare_op != SPLC_OP_NONE &&
            !compare_ok((SPLC_CompareOp)rule->compare_op, current, rule->threshold_lo, rule->threshold_hi)) {
            rt->state = RULE_STATE_BLOCKED;
            return false;
        }
        rt->state = RULE_STATE_COMPARED;
    }
    /* FALLTHROUGH */

    case RULE_STATE_COMPARED: {
        if (is_edge_type && rule->for_ms > 0) {
            rt->dwell_start_tick = now_ms;
            rt->state = RULE_STATE_DWELLING;
            /* Dwell just started: do not fire yet, wait for a later scan. */
            return false;
        }
        /* No dwell needed: proceed straight to Guard within this scan. */
        rt->state = RULE_STATE_GUARD_CHECK;
    }
    /* FALLTHROUGH */

    case RULE_STATE_DWELLING: {
        /* THE ONLY STATE THAT "LIVES" ACROSS MULTIPLE CALLS.
         * Every scan cycle, re-check whether the current LEVEL still
         * matches the meaning of the trigger.
         *
         * FIXED BUG: an earlier version relied solely on compare_ok(),
         * but compare_op is typically SPLC_OP_NONE for edge-type rules
         * (e.g. ON_FALL with no threshold at all) -- "OP_NONE => always
         * considered still holding" meant dwell was never cancelled even
         * if the underlying signal flipped back mid-dwell. Fix: for
         * is_edge_type, "level still holds" must be derived directly from
         * current relative to the direction of trigger_type (ON_FALL =>
         * still holds means current stays ==0; ON_RISE => stays !=0),
         * THEN compare_ok() is applied on top as a secondary condition
         * only if the rule declares a compare_op.
         */
        bool level_still_holds;
        if (is_edge_type) {
            bool trigger_level_holds =
                (trigger_type == SPLC_TRG_ON_FALL)  ? (current == 0) :
                (trigger_type == SPLC_TRG_ON_RISE)  ? (current != 0) :
                                                       true; /* ON_CHANGE: no fixed "level" to hold */
            level_still_holds = trigger_level_holds &&
                ((rule->compare_op == SPLC_OP_NONE) ||
                 compare_ok((SPLC_CompareOp)rule->compare_op, current, rule->threshold_lo, rule->threshold_hi));
        } else {
            level_still_holds = true;
        }

        if (!level_still_holds) {
            /* Condition was interrupted mid-dwell -> cancel dwell, back to BLOCKED.
             * FIXED BUG: prev_value must be updated on EVERY exit path out
             * of this function, not only in the IDLE/BLOCKED case --
             * otherwise the next call would compare current against a
             * stale prev_value (from before dwell started), losing a new
             * edge that occurs right after dwell is cancelled. */
            rt->prev_value       = current;
            rt->dwell_start_tick = DWELL_NOT_STARTED;
            rt->state = RULE_STATE_BLOCKED;
            return false;
        }
        /*
         * Only edge-type rules with for_ms > 0 ever arm the dwell timer
         * (see RULE_STATE_COMPARED above); INTERVAL / TIME_WINDOW rules
         * reach this point with dwell_start_tick still == DWELL_NOT_STARTED
         * (0xFFFFFFFF). Without the "armed" test below, the unsigned
         * subtraction `now_ms - 0xFFFFFFFF` equals now_ms + 1, which is
         * < for_ms whenever the tick counter is small -- i.e. right after
         * it wraps past UINT32_MAX (~49.7 days) -- so a due INTERVAL rule
         * was mistaken for "dwell not finished yet" and delayed one scan
         * (or longer). The interval itself is already checked by
         * trigger_timing_ok() in the IDLE/BLOCKED case, so it needs no
         * second time check here.
         */
        if (rt->dwell_start_tick != DWELL_NOT_STARTED &&
            (uint32_t)(now_ms - rt->dwell_start_tick) < rule->for_ms) {
            /* Not enough time yet, STAY in state = DWELLING, wait for next scan. */
            rt->prev_value = current;
            return false;
        }
        /* Enough time elapsed: proceed to Guard within this same scan. */
        rt->state = RULE_STATE_GUARD_CHECK;
    }
    /* FALLTHROUGH */

    case RULE_STATE_GUARD_CHECK: {
        uint16_t guard_idx = rule->guard_tag & GUARD_TAG_INDEX_MASK;
        bool     negate    = (rule->guard_tag & GUARD_TAG_NEGATE_BIT) != 0;
        /*
         * FIXED BUG: this used to compare guard_idx against TAG_NONE (0),
         * which was correct under the v1.7 tag layout (index 0 was a
         * reserved, meaningless sentinel slot) but became wrong once
         * plc_tag_def.h moved to the v1.9 layout, where index 0 is
         * TAG_DI0 -- a real tag. That made TAG_DI0 the one tag in the
         * whole system that could never be used as a guard: any rule
         * with guard_tag = TAG_DI0 (0) had its guard silently treated as
         * "absent" and always passed, regardless of DI0's actual value.
         * Verified by an actual compiled/run test before this fix.
         * Fix: compare against GUARD_TAG_NONE (0x7FFF) instead, a value
         * no real tag index can ever reach (see plc_rule.h).
         */
        bool     guard_open = (guard_idx == GUARD_TAG_NONE) ||
                               (negate ? (tag_read(guard_idx) == 0) : (tag_read(guard_idx) != 0));

        if (!guard_open) {
            rt->state = RULE_STATE_BLOCKED;
            return false;
        }
        rt->state = RULE_STATE_FIRE;
    }
    /* FALLTHROUGH */

    case RULE_STATE_FIRE: {
        execute_action(rule);
        rt->last_fire_tick   = now_ms;
        rt->dwell_start_tick = DWELL_NOT_STARTED;
        rt->state = RULE_STATE_IDLE;
        log_debug(TAG, "rule[%d] FIRED: trigger_tag=%u action_type=%u action_tag=%u action_param=%ld",
                  (int)(rule - g_rule_table), rule->trigger_tag, rule->action_type,
                  rule->action_tag, (long)rule->action_param);
        return true;
    }

    default:
        rt->state = RULE_STATE_IDLE;
        return false;
    }

    return false;
}