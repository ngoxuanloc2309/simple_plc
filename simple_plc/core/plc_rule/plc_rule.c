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
 * dwell/debounce behavior without crashing (TRG_INTERVAL never reaches its
 * period, dwell never reaches for_ms).
 */
static uint32_t get_system_tick_ms(void)
{
    return 0;
}

/*
 * TODO: no RTC/calendar source is wired up in Layer 2 (by design -- Layer 2
 * must stay hardware-free, see docs/architecture.md section 2/"ranh giới
 * port"). TRG_TIME_WINDOW (spec section 4.2/4.4) needs current
 * time-of-day as HHMM to compare against threshold_lo/threshold_hi. Until
 * Layer 4 injects a real wall-clock source (e.g. via a function pointer
 * set at plc_engine_init()), this returns 0, which means TRG_TIME_WINDOW
 * rules will not fire correctly -- they will not crash, but they are not
 * functional yet. This mirrors the same stub pattern already used by
 * get_system_tick_ms() above.
 */
static uint32_t get_wallclock_hhmm(void)
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

/*
 * Sentinel used in RuleRuntime.condition_since_tick to mean "dwell timer
 * not currently running". Using this instead of the bare value 0 is a
 * FIX: 0 is also a perfectly valid, reachable system tick (e.g. the first
 * scan cycle after boot, or whenever get_system_tick_ms() legitimately
 * wraps back to 0). The previous code tested `condition_since_tick == 0`
 * to mean "not yet started", so any dwell window that genuinely began at
 * tick 0 was indistinguishable from "not started" and could never reach
 * its for_ms deadline. UINT32_MAX is used instead because for_ms is a
 * u32 duration in ms (spec section 4.2); a dwell window starting at
 * UINT32_MAX and needing to survive an additional for_ms would overflow
 * before it could be confused with a real in-progress window.
 */
#define DWELL_NOT_STARTED  0xFFFFFFFFu

/*
 * Dwell/debounce check for edge-based triggers only (TRG_ON_CHANGE/
 * ON_RISE/ON_FALL). Per spec section 4.2, for_ms means "must hold
 * continuously for this many ms before counting as triggered" for these
 * three trigger types.
 *
 * FIX: previously this was called unconditionally for every trigger type,
 * including TRG_INTERVAL/TRG_TIME_WINDOW, with condition_met hardcoded to
 * true. For an edge trigger, condition_met is only ever true on the single
 * scan cycle where the edge itself occurs (e.g. ON_FALL: prev!=0 &&
 * current==0) -- current is a transition, not a level, so on the very
 * next scan cycle the tag has already settled and check_trigger_edge()
 * returns false again. That meant condition_since_tick was set once and
 * then immediately reset to 0 on the following cycle, so
 * "(now_ms - condition_since_tick) >= for_ms" could never become true --
 * any rule with for_ms > 0 on an edge trigger (e.g. spec 4.4 R1: ON_FALL,
 * for_ms=300000) would never fire. Dwell for an edge trigger has to be
 * measured against the CURRENT LEVEL implied by the edge (e.g. "has the
 * tag stayed at 0 since it fell") using compare_ok()'s result rather than
 * the edge-detect result; see the call in rule_scan() below.
 */
static bool dwell_ok(RuleRuntime *runtime, bool condition_met, uint32_t for_ms, uint32_t now_ms)
{
    if (for_ms == 0) {
        return condition_met;
    }

    if (!condition_met) {
        runtime->condition_since_tick = DWELL_NOT_STARTED;
        return false;
    }

    if (runtime->condition_since_tick == DWELL_NOT_STARTED) {
        runtime->condition_since_tick = now_ms;
        return false;
    }

    return (now_ms - runtime->condition_since_tick) >= for_ms;
}

/*
 * FIX: factored out so both rule_table_load_from_flash() and
 * rule_table_commit() reset runtime state identically. Previously both
 * used memset(...,0,...), which left condition_since_tick at 0 -- the
 * exact value dwell_ok() used to mean "not started" -- so a freshly
 * loaded/committed rule with for_ms > 0 looked like it already had a
 * dwell window open starting at tick 0 instead of "no window yet". Must
 * be set to DWELL_NOT_STARTED (see dwell_ok() above) explicitly; a plain
 * memset cannot express that since it isn't 0.
 */
static void reset_all_rule_runtime(void)
{
    for (int i = 0; i < MAX_RULES; i++) {
        g_rule_runtime[i].prev_value           = 0;
        g_rule_runtime[i].condition_since_tick = DWELL_NOT_STARTED;
        g_rule_runtime[i].last_fire_tick       = 0;
    }
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
    reset_all_rule_runtime();
    g_rule_count = 0;
}

/*
 * FIX: rebuilt to match the step order in spec section 6.5 / architecture.md
 * section 2.2: enabled -> Trigger -> Compare -> Dwell(for_ms) -> Guard ->
 * execute_action(), "continue" (skip to next rule) the instant any step
 * fails. The previous version diverged from that in three ways, all
 * corrected below:
 *
 * 1. TRG_TIME_WINDOW/TRG_INTERVAL are not edges; they are now routed to
 *    trigger_timing_ok() instead of being forced through edge-detection
 *    with Rule.for_ms double-booked as their period (see plc_rule_eval.*).
 * 2. dwell_ok() is now only invoked for the three edge trigger types, and
 *    is fed the LEVEL implied by the edge (via compare_ok()-equivalent
 *    logic below), not the one-shot edge-detect result -- otherwise
 *    condition_since_tick was reset to 0 on the very next cycle and
 *    for_ms could never elapse (see the comment on dwell_ok() above).
 * 3. TRG_TIME_WINDOW/TRG_INTERVAL rules no longer get an extra, spurious
 *    dwell_ok() pass on top of trigger_timing_ok(); the spec only applies
 *    for_ms/dwell semantics to the three edge trigger types (section 4.2).
 */
void rule_scan(void)
{
    uint32_t now_ms   = get_system_tick_ms();
    uint32_t now_hhmm = get_wallclock_hhmm();

    for (int i = 0; i < g_rule_count; i++) {
        Rule        *rule    = &g_rule_table[i];
        RuleRuntime *runtime = &g_rule_runtime[i];

        if (!rule->enabled) {
            continue;
        }

        TriggerType trigger_type = (TriggerType)rule->trigger_type;
        int32_t     current      = tag_read(rule->trigger_tag);
        bool        is_edge_type = (trigger_type == TRG_ON_CHANGE ||
                                     trigger_type == TRG_ON_RISE   ||
                                     trigger_type == TRG_ON_FALL);

        /* --- Step 1: Trigger --- */
        bool trigger_met;
        if (is_edge_type) {
            trigger_met = check_trigger_edge(trigger_type, runtime->prev_value, current);
        } else {
            trigger_met = trigger_timing_ok(trigger_type,
                                             now_ms,
                                             now_hhmm,
                                             rule->threshold_lo,
                                             rule->threshold_hi,
                                             rule->for_ms,
                                             runtime->last_fire_tick);
        }
        /* prev_value must update every scan regardless of trigger_met, or
         * the next edge could not be detected relative to this cycle. */
        runtime->prev_value = current;

        if (!trigger_met) {
            /* No trigger this cycle: an edge rule with an active dwell
             * countdown must not have it silently reset here -- the level
             * may still be held. Only compare_ok() (step 2, evaluated
             * against the CURRENT value every cycle) determines whether
             * the dwell timer keeps running or resets; see step 3. */
            if (is_edge_type && rule->for_ms > 0) {
                bool level_held = compare_ok((CompareOp)rule->compare_op, current,
                                              rule->threshold_lo, rule->threshold_hi);
                dwell_ok(runtime, level_held, rule->for_ms, now_ms);
            }
            continue;
        }

        /* --- Step 2: Compare --- */
        if (rule->compare_op != OP_NONE &&
            !compare_ok((CompareOp)rule->compare_op, current, rule->threshold_lo, rule->threshold_hi)) {
            continue;
        }

        /* --- Step 3: Dwell (edge triggers only; timing triggers have no
         * separate dwell concept -- trigger_timing_ok() already gated
         * them on for_ms as their period). --- */
        if (is_edge_type && rule->for_ms > 0) {
            if (!dwell_ok(runtime, true, rule->for_ms, now_ms)) {
                continue;
            }
        }

        /* --- Step 4: Guard --- */
        if (!guard_ok(rule->guard_tag)) {
            continue;
        }

        /* --- Step 5: execute_action() --- */
        execute_action(rule);
        runtime->last_fire_tick     = now_ms;
        runtime->condition_since_tick = 0;
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
    reset_all_rule_runtime();  /* FIX: see reset_all_rule_runtime() above */
    g_rule_count = rule_count;

    return 0;
}