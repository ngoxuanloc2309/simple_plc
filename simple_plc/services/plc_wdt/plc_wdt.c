/*
 * plc_wdt.c - Layer 3 (Services). See plc_wdt.h and docs/SPLC_Watchdog_Design.md.
 */
#include "plc_wdt.h"

#if SPLC_WDG_ENABLE

#include "splc_log.h"
#include "sx_time.h"

static const char *TAG = "PLC_WDT";

typedef struct {
    const char       *name;
    uint32_t          timeout_ms;
    volatile uint32_t deadline_ms;   /* tick at which the channel counts as late */
    volatile bool     used;
} plc_wdt_channel_t;

static plc_wdt_channel_t s_ch[PLC_WDT_CH_COUNT];
static plc_wdt_kick_fn   s_kick;
static plc_wdt_fault_fn  s_fault;
static volatile bool     s_tripped;
static bool              s_no_kick_logged;

/* a is later than b (wrap-safe for gaps below 2^31 ms) */
static inline bool tick_after(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) > 0;
}

bool plc_wdt_register(plc_wdt_ch_t ch, const char *name, uint32_t timeout_ms, uint32_t boot_grace_ms)
{
    if ((unsigned)ch >= (unsigned)PLC_WDT_CH_COUNT || timeout_ms == 0U) {
        return false;
    }
    plc_wdt_channel_t *c = &s_ch[ch];
    const uint32_t first = (boot_grace_ms > timeout_ms) ? boot_grace_ms : timeout_ms;

    c->used = false;                     /* the supervisor skips it while it is rewritten */
    c->name = (name != NULL) ? name : "?";
    c->timeout_ms = timeout_ms;
    c->deadline_ms = sx_get_tick_ms() + first;
    c->used = true;
    return true;
}

void plc_wdt_alive(plc_wdt_ch_t ch)
{
    if ((unsigned)ch >= (unsigned)PLC_WDT_CH_COUNT) {
        return;
    }
    plc_wdt_channel_t *c = &s_ch[ch];
    if (c->used) {
        c->deadline_ms = sx_get_tick_ms() + c->timeout_ms;
    }
}

void plc_wdt_grace(plc_wdt_ch_t ch, uint32_t ms)
{
    if ((unsigned)ch >= (unsigned)PLC_WDT_CH_COUNT) {
        return;
    }
    plc_wdt_channel_t *c = &s_ch[ch];
    if (c->used) {
        const uint32_t until = sx_get_tick_ms() + ms;
        if (tick_after(until, c->deadline_ms)) {
            c->deadline_ms = until;
        }
    }
}

void plc_wdt_set_kick_fn(plc_wdt_kick_fn fn)   { s_kick = fn; }
void plc_wdt_set_fault_fn(plc_wdt_fault_fn fn) { s_fault = fn; }
bool plc_wdt_is_tripped(void)                  { return s_tripped; }

void plc_wdt_service(void)
{
    if (s_tripped) {
        return;                          /* latched: no more kicks, the hardware watchdog resets */
    }

    const uint32_t now = sx_get_tick_ms();
    for (unsigned i = 0U; i < (unsigned)PLC_WDT_CH_COUNT; i++) {
        const plc_wdt_channel_t *c = &s_ch[i];
        if (!c->used || !tick_after(now, c->deadline_ms)) {
            continue;
        }
        const uint32_t late = now - c->deadline_ms;
        s_tripped = true;
        log_error(TAG, "channel %s: no sign of life, %u ms past its deadline: kicking stops, "
                       "the hardware watchdog will reset the board", c->name, (unsigned)late);
        if (s_fault != NULL) {
            s_fault(c->name, late);
        }
        return;
    }

    if (s_kick != NULL) {
        s_kick();
    } else if (!s_no_kick_logged) {
        s_no_kick_logged = true;
        log_warn(TAG, "no kick function registered: the hardware watchdog will reset the board");
    }
}

#endif /* SPLC_WDG_ENABLE */