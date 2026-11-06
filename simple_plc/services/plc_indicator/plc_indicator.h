#ifndef PLC_INDICATOR_H
#define PLC_INDICATOR_H

/*
 * plc_indicator.h - Layer 3 (PLC Application Services)
 *
 * Connection indicator for three LEDs: POWER, STATUS, LINK (docs/plc_indicator_design.md,
 * handoff decision 99). The library decides what each LED shows; it knows no GPIO, no
 * active level, no LED type. The PRODUCT writes a port file (outside the library, an OBJECT
 * library like Zigbee_IO/port) that
 *   1. gives the on/off function            -> splc_ind_ops_t
 *   2. gives one get_state() per protocol   -> splc_ind_proto_t (non-blocking, no I/O wait)
 *   3. registers the protocols in priority order (the order of the calls)
 *   4. calls splc_indicator_init() then splc_indicator_register() ... before the first scan.
 *
 * What the LEDs show (the library's constants, the same on every product):
 *   POWER   lit from splc_indicator_init().
 *   Nothing registered: only POWER is lit.
 *   Every enabled protocol CONNECTED: STATUS and LINK steady on.
 *   Otherwise the REPORTED set = enabled, not CONNECTED and not hidden by depends_on.
 *     LINK   : the pattern of the protocol being shown (the product's choice).
 *     STATUS : CONNECTING flashes once per SPLC_IND_STATUS_CONNECTING_MS,
 *              FAILED once per SPLC_IND_STATUS_FAILED_MS.
 *     Several protocols in the set take turns, SPLC_IND_SLOT_MS each (rounded up to whole
 *     pattern cycles), with SPLC_IND_GAP_MS of BOTH LEDs off between two of them. One
 *     protocol alone is shown continuously, with no gap.
 *
 * Pull model: splc_indicator_scan() reads the states every engine cycle, so a port never
 * calls back into the library (no ISR use). Single context, no heap, no locking.
 * With SPLC_INDICATOR_ENABLE = 0 this whole module is empty.
 */

#include <stdbool.h>
#include <stdint.h>

#include "splc_opt.h"
#include "plc_indicator_err_define.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Sets one LED. GPIO write only: no waiting, no logging. `led` is an SPLC_LED_ID. The
 * library calls it only when a level really changes. */
typedef struct {
    void (*set)(SPLC_LED_ID led, bool on, void *ctx);
    void  *ctx;
} splc_ind_ops_t;

/* One protocol (one row of the table). */
typedef struct {
    const char         *name;        /* log only; may be NULL                                  */
    splc_led_pattern_t  pattern;     /* LED LINK pattern while this protocol is shown          */
    SPLC_LINK_STATE   (*get_state)(void *ctx);   /* required; non-blocking                     */
    void               *ctx;
    uint8_t             depends_on;  /* row index + 1 of an EARLIER row, 0 = none. While that
                                      * row is CONNECTING / FAILED this one is hidden (Wi-Fi
                                      * down: MQTT cannot work, reporting both is noise).     */
} splc_ind_proto_t;

#if SPLC_INDICATOR_ENABLE

/* Install the LED function and light POWER. false when `ops` or ops->set is NULL. Call once,
 * before any register. */
bool splc_indicator_init(const splc_ind_ops_t *ops);

/* Add a protocol; the call order is the priority. Returns the row index (use index + 1 for a
 * later row's depends_on), or -1 when refused: no init yet, table closed (the first scan
 * already ran), table full (SPLC_INDICATOR_MAX_PROTOCOLS), no get_state, bad pattern, or
 * depends_on not naming an earlier row. Refusals are logged. */
int splc_indicator_register(const splc_ind_proto_t *p);

/* Once per engine cycle with the cycle's tick (ms). Runs even while a diagnostic session
 * holds the rule engine: the LEDs must keep reporting connections. */
void splc_indicator_scan(uint32_t now_ms);

/* Row index currently shown on LED LINK, or -1 (steady, in a gap, or nothing registered).
 * For tests and diagnostics. */
int splc_indicator_shown(void);

#endif /* SPLC_INDICATOR_ENABLE */

#ifdef __cplusplus
}
#endif

#endif /* PLC_INDICATOR_H */