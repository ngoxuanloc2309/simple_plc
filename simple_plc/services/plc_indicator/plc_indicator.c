/*
 * plc_indicator.c - Layer 3 (PLC Application Services)
 *
 * See plc_indicator.h and docs/plc_indicator_design.md. The whole file is empty unless
 * SPLC_INDICATOR_ENABLE is 1.
 */

#include "plc_indicator.h"

#if SPLC_INDICATOR_ENABLE

#include <string.h>

#include "splc_log.h"

#define TAG "IND"

typedef enum {
    MODE_STEADY = 0,   /* nothing to report (or nothing registered): fixed LED levels      */
    MODE_SHOW,         /* LEDs report row `cur`                                            */
    MODE_GAP           /* both LEDs off between two rows; `prev` was the row shown before  */
} ind_mode_t;

static struct {
    splc_ind_ops_t     ops;
    bool               have_ops;
    bool               started;               /* first scan ran: the table is closed */
    splc_ind_proto_t   proto[SPLC_INDICATOR_MAX_PROTOCOLS];
    uint8_t            n;

    SPLC_LINK_STATE    state[SPLC_INDICATOR_MAX_PROTOCOLS];   /* sampled this scan */

    /* LED output cache: ops.set() is called only when a level really changes */
    bool               led_known[SPLC_LED_COUNT];
    bool               led_on[SPLC_LED_COUNT];

    ind_mode_t         mode;
    int8_t             cur;
    int8_t             prev;
    uint32_t           gap_start;

    uint32_t           anchor;                /* LINK pattern phase origin                    */
    bool               slot_end_valid;
    uint32_t           slot_end_off;          /* slot ends when (now - anchor) >= this        */

    uint32_t           st_anchor;             /* STATUS blink phase origin                    */
    SPLC_LINK_STATE    st_state;              /* state the STATUS phase was started for       */
} s;

/* ---- small helpers ------------------------------------------------------ */

static void led_write(SPLC_LED_ID id, bool on)
{
    if (s.led_known[id] && (s.led_on[id] == on)) {
        return;
    }
    s.led_known[id] = true;
    s.led_on[id] = on;
    s.ops.set(id, on, s.ops.ctx);
}

static uint32_t pattern_cycle(const splc_led_pattern_t *p)
{
    return ((uint32_t)p->count * ((uint32_t)p->on_ms + (uint32_t)p->off_ms)) + (uint32_t)p->pause_ms;
}

/* LED level `el` ms after the pattern started: `count` flashes of on_ms, each followed by off_ms,
 * then pause_ms off. */
static bool pattern_on(const splc_led_pattern_t *p, uint32_t el)
{
    uint32_t group = (uint32_t)p->on_ms + (uint32_t)p->off_ms;
    uint32_t phase = el % pattern_cycle(p);

    if (phase >= ((uint32_t)p->count * group)) {
        return false;                          /* the pause after the group */
    }
    return (phase % group) < (uint32_t)p->on_ms;
}

static bool pattern_valid(const splc_led_pattern_t *p)
{
    if (p->on_ms == 0U || p->count == 0U) {
        return false;
    }
    if (p->count > 1U && p->off_ms == 0U) {
        return false;                          /* the flashes would run together */
    }
    return true;
}

static const char *state_name(SPLC_LINK_STATE st)
{
    switch (st) {
    case SPLC_LINK_STATE_DISABLED:   return "disabled";
    case SPLC_LINK_STATE_CONNECTING: return "connecting";
    case SPLC_LINK_STATE_CONNECTED:  return "connected";
    default:                         return "failed";
    }
}

static const char *row_name(int i)
{
    return ((i >= 0) && (i < (int)s.n) && (s.proto[i].name != NULL)) ? s.proto[i].name : "?";
}

/* Sample every row, then return the bit mask of the rows that must be reported:
 * enabled, not CONNECTED, and not hidden by a dependency. */
static uint8_t sample_and_mask(void)
{
    uint8_t mask = 0U;
    uint8_t i;

    for (i = 0U; i < s.n; i++) {
        SPLC_LINK_STATE st = s.proto[i].get_state(s.proto[i].ctx);
        if ((unsigned)st > (unsigned)SPLC_LINK_STATE_FAILED) {
            st = SPLC_LINK_STATE_FAILED;       /* a port returning garbage is shown as a failure */
        }
        s.state[i] = st;
    }
    for (i = 0U; i < s.n; i++) {
        bool report = (s.state[i] == SPLC_LINK_STATE_CONNECTING) || (s.state[i] == SPLC_LINK_STATE_FAILED);

        if (report && (s.proto[i].depends_on != 0U)) {
            SPLC_LINK_STATE ps = s.state[s.proto[i].depends_on - 1U];   /* always an earlier row */
            if ((ps == SPLC_LINK_STATE_CONNECTING) || (ps == SPLC_LINK_STATE_FAILED)) {
                report = false;                /* the base link is down: this one cannot work */
            }
        }
        if (report) {
            mask |= (uint8_t)(1U << i);
        }
    }
    return mask;
}

static uint8_t popcount8(uint8_t v)
{
    uint8_t c = 0U;
    while (v != 0U) {
        c += (uint8_t)(v & 1U);
        v >>= 1;
    }
    return c;
}

/* First row of `mask` after row `after` (cyclic, `after` itself is the last candidate);
 * after = -1 starts at row 0. -1 when the mask is empty. */
static int next_row(uint8_t mask, int after)
{
    int k;
    for (k = 1; k <= (int)s.n; k++) {
        int i = (after < 0) ? (k - 1) : ((after + k) % (int)s.n);
        if ((mask & (1U << i)) != 0U) {
            return i;
        }
    }
    return -1;
}

/* Round "SLOT_MS from `el` on" up to a whole number of pattern cycles (offset from `anchor`). */
static void slot_arm(uint32_t el)
{
    uint32_t cycle = pattern_cycle(&s.proto[s.cur].pattern);

    s.slot_end_off = (((el + SPLC_IND_SLOT_MS) + cycle - 1U) / cycle) * cycle;
    s.slot_end_valid = true;
}

/* `nvis` > 1: several rows take turns, so the slot clock starts together with the pattern. */
static void start_show(int row, uint32_t now, uint8_t nvis)
{
    s.mode = MODE_SHOW;
    s.cur = (int8_t)row;
    s.anchor = now;
    s.st_anchor = now;
    s.st_state = s.state[row];
    s.slot_end_valid = false;
    if (nvis > 1U) {
        slot_arm(0U);
    }
    log_info(TAG, "show %s (%s)", row_name(row), state_name(s.state[row]));
}

static void begin_gap(uint32_t now)
{
    s.mode = MODE_GAP;
    s.prev = s.cur;
    s.cur = -1;
    s.gap_start = now;
}

/* Keep `anchor` close to `now` (a multiple of the cycle behind it) so the 32-bit tick wrap
 * never matters after weeks of showing one protocol. The phase is unchanged. */
static void reanchor(uint32_t now, uint32_t cycle)
{
    uint32_t el = (uint32_t)(now - s.anchor);
    s.anchor += (el / cycle) * cycle;
}

static void render(uint32_t now)
{
    const splc_ind_proto_t *p = &s.proto[s.cur];
    SPLC_LINK_STATE st = s.state[s.cur];
    uint32_t period;
    uint32_t el;

    led_write(SPLC_LED_LINK, pattern_on(&p->pattern, (uint32_t)(now - s.anchor)));

    /* STATUS: a change of state restarts its phase, so the new rhythm starts with a flash. */
    if (st != s.st_state) {
        s.st_state = st;
        s.st_anchor = now;
    }
    period = (st == SPLC_LINK_STATE_CONNECTING) ? SPLC_IND_STATUS_CONNECTING_MS
                                                : SPLC_IND_STATUS_FAILED_MS;
    el = (uint32_t)(now - s.st_anchor);
    led_write(SPLC_LED_STATUS, (el % period) < SPLC_LED_FLASH_ON_MS);
    s.st_anchor += (el / period) * period;
}

/* ---- public API --------------------------------------------------------- */

bool splc_indicator_init(const splc_ind_ops_t *ops)
{
    if ((ops == NULL) || (ops->set == NULL)) {
        return false;
    }
    memset(&s, 0, sizeof(s));
    s.ops = *ops;
    s.have_ops = true;
    s.cur = -1;
    s.prev = -1;

    led_write(SPLC_LED_POWER, true);
    led_write(SPLC_LED_STATUS, false);
    led_write(SPLC_LED_LINK, false);
    return true;
}

int splc_indicator_register(const splc_ind_proto_t *p)
{
    if (!s.have_ops) {
        return -1;                             /* splc_indicator_init() first */
    }
    if (s.started) {
        log_error(TAG, "register: table closed (first scan already ran)");
        return -1;
    }
    if ((p == NULL) || (p->get_state == NULL)) {
        log_error(TAG, "register: no get_state");
        return -1;
    }
    if (s.n >= SPLC_INDICATOR_MAX_PROTOCOLS) {
        log_error(TAG, "register: table full (%u rows)", (unsigned)SPLC_INDICATOR_MAX_PROTOCOLS);
        return -1;
    }
    if (!pattern_valid(&p->pattern)) {
        log_error(TAG, "register %s: bad LINK pattern", (p->name != NULL) ? p->name : "?");
        return -1;
    }
    if (p->depends_on > s.n) {
        log_error(TAG, "register %s: depends_on must name an earlier row", (p->name != NULL) ? p->name : "?");
        return -1;
    }
    s.proto[s.n] = *p;
    return (int)s.n++;
}

void splc_indicator_scan(uint32_t now_ms)
{
    uint8_t mask;
    uint8_t nvis;

    if (!s.have_ops) {
        return;
    }
    s.started = true;
    if (s.n == 0U) {
        return;                                /* nothing registered: only POWER is lit */
    }

    mask = sample_and_mask();
    nvis = popcount8(mask);

    if (nvis == 0U) {
        if (s.mode != MODE_STEADY) {
            log_info(TAG, "all connections OK");
        }
        s.mode = MODE_STEADY;
        s.cur = -1;
        led_write(SPLC_LED_STATUS, true);
        led_write(SPLC_LED_LINK, true);
        return;
    }

    if (s.mode == MODE_STEADY) {
        start_show(next_row(mask, -1), now_ms, nvis);
    } else if ((s.mode == MODE_SHOW) && ((mask & (1U << s.cur)) == 0U)) {
        begin_gap(now_ms);                     /* the shown one is OK now (or hidden): next one, after a gap */
    }

    if ((s.mode == MODE_GAP) && ((uint32_t)(now_ms - s.gap_start) >= SPLC_IND_GAP_MS)) {
        start_show(next_row(mask, s.prev), now_ms, nvis);
    }

    if (s.mode == MODE_SHOW) {
        uint32_t cycle = pattern_cycle(&s.proto[s.cur].pattern);

        if (nvis > 1U) {
            if (!s.slot_end_valid) {           /* a second protocol appeared while this one was alone */
                slot_arm((uint32_t)(now_ms - s.anchor));
            }
            if ((uint32_t)(now_ms - s.anchor) >= s.slot_end_off) {
                begin_gap(now_ms);
                if (SPLC_IND_GAP_MS == 0U) {   /* no gap: go straight to the next one */
                    start_show(next_row(mask, s.prev), now_ms, nvis);
                }
            }
        } else {
            s.slot_end_valid = false;          /* alone: shown continuously, no rotation */
            reanchor(now_ms, cycle);
        }
    }

    if (s.mode == MODE_GAP) {
        led_write(SPLC_LED_STATUS, false);
        led_write(SPLC_LED_LINK, false);
        return;
    }
    render(now_ms);
}

int splc_indicator_shown(void)
{
    return (s.mode == MODE_SHOW) ? (int)s.cur : -1;
}

#endif /* SPLC_INDICATOR_ENABLE */