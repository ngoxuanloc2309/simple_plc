/*
 * plc_wdt.h - Layer 3 (Services)
 *
 * Software supervisor in front of a hardware watchdog (IWDG). Design: docs/SPLC_Watchdog_Design.md.
 *
 * The library knows no watchdog hardware. The host registers ONE kick function
 * (plc_wdt_set_kick_fn(), e.g. HAL_IWDG_Refresh) and calls plc_wdt_service() periodically from a
 * task that keeps running while the others hang. plc_wdt_service() calls the kick function only
 * while EVERY registered channel has reported plc_wdt_alive() inside its deadline. The first
 * channel that misses its deadline trips the supervisor: it logs, calls the fault function,
 * and never kicks again (latched), so the hardware watchdog resets the board a moment later.
 *
 * Channels (one per supervised activity):
 *   PLC_WDT_CH_PLC    registered and fed by the library itself (plc_engine_init / plc_engine_poll)
 *   PLC_WDT_CH_USER0..2   free for the host (e.g. the Wi-Fi/ST67 task)
 *
 * Threading: a channel must be driven from ONE task (alive / grace are plain 32-bit stores, no
 * lock). plc_wdt_service() may run in another task. Not callable from an ISR except
 * plc_wdt_alive().
 *
 * Compiled out with SPLC_WDG_ENABLE = 0 (config/splc_opt.h): every function below is an empty
 * inline, no code, no RAM.
 */
#ifndef PLC_WDT_H
#define PLC_WDT_H

#include <stdbool.h>
#include <stdint.h>

#include "splc_opt.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PLC_WDT_CH_PLC = 0,   /* the engine task: owned by the library */
    PLC_WDT_CH_USER0,
    PLC_WDT_CH_USER1,
    PLC_WDT_CH_USER2,
    PLC_WDT_CH_COUNT
} plc_wdt_ch_t;

/* Feeds the hardware watchdog (host: HAL_IWDG_Refresh). Called from plc_wdt_service() only. */
typedef void (*plc_wdt_kick_fn)(void);

/* Called once, from plc_wdt_service(), when a channel missed its deadline and kicking stops.
 * name: channel name given at registration. late_ms: how long past the deadline it is.
 * Keep it short and non-blocking (store the cause somewhere that survives a reset). */
typedef void (*plc_wdt_fault_fn)(const char *name, uint32_t late_ms);

#if SPLC_WDG_ENABLE

/* Registers (or re-registers) a channel. name is kept by pointer (use a string literal).
 * timeout_ms: longest allowed gap between two plc_wdt_alive() calls.
 * boot_grace_ms: time allowed until the FIRST plc_wdt_alive(); the longer of the two is used.
 * Returns false for a bad channel or timeout_ms = 0. */
bool plc_wdt_register(plc_wdt_ch_t ch, const char *name, uint32_t timeout_ms, uint32_t boot_grace_ms);

/* The activity of the channel made progress: deadline = now + timeout_ms.
 * An unregistered channel is ignored. */
void plc_wdt_alive(plc_wdt_ch_t ch);

/* A known long operation starts: the deadline is moved out to at least now + ms. It never
 * shortens the deadline, and the next plc_wdt_alive() sets the normal deadline again. */
void plc_wdt_grace(plc_wdt_ch_t ch, uint32_t ms);

void plc_wdt_set_kick_fn(plc_wdt_kick_fn fn);
void plc_wdt_set_fault_fn(plc_wdt_fault_fn fn);

/* Checks every channel and kicks when all are inside their deadline. Call every
 * SPLC_WDG_SERVICE_MS from a task that is not one of the supervised ones. */
void plc_wdt_service(void);

/* True once a channel has missed its deadline (latched until reset). */
bool plc_wdt_is_tripped(void);

#else  /* SPLC_WDG_ENABLE == 0: no code */

static inline bool plc_wdt_register(plc_wdt_ch_t ch, const char *name, uint32_t timeout_ms, uint32_t boot_grace_ms)
{ (void)ch; (void)name; (void)timeout_ms; (void)boot_grace_ms; return true; }
static inline void plc_wdt_alive(plc_wdt_ch_t ch) { (void)ch; }
static inline void plc_wdt_grace(plc_wdt_ch_t ch, uint32_t ms) { (void)ch; (void)ms; }
static inline void plc_wdt_set_kick_fn(plc_wdt_kick_fn fn) { (void)fn; }
static inline void plc_wdt_set_fault_fn(plc_wdt_fault_fn fn) { (void)fn; }
static inline void plc_wdt_service(void) {}
static inline bool plc_wdt_is_tripped(void) { return false; }

#endif /* SPLC_WDG_ENABLE */

#ifdef __cplusplus
}
#endif

#endif /* PLC_WDT_H */