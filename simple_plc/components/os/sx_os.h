#ifndef SX_OS_H
#define SX_OS_H

/*
 * sx_os.h - Layer 1 contract: the few OS services the library needs.
 * Implemented in Layer 0: platforms/freertos/sx_os_freertos.c (to support
 * another RTOS, add a sibling platforms/<rtos>/ and select it in CMake).
 *
 * With SX_OS_USE_FREERTOS = 0 (app/sx_os_config.h) everything here is an
 * inline no-op, so bare-metal behaviour is exactly what it was before.
 *
 * With SX_OS_USE_FREERTOS = 1 the implementation (platforms/freertos/
 * sx_os_freertos.c) uses FreeRTOS. What the host must provide:
 *   - FreeRTOS.h / task.h on the include path (CMake: SPLC_OS_INCLUDE_DIRS),
 *   - FreeRTOSConfig.h with INCLUDE_vTaskDelay = 1 and
 *     INCLUDE_xTaskGetSchedulerState = 1,
 *     and configSUPPORT_DYNAMIC_ALLOCATION = 1 (the logger mutex),
 *   - ONE task that calls plc_engine_init() and then plc_engine_poll()
 *     (+ a short vTaskDelay between polls). The library is single-threaded
 *     by design: tags, rule tables and the Modbus draft have no locks.
 *     Other tasks must not touch them.
 *   - HAL_GetTick() must keep advancing in 1 ms steps. sx_get_tick_ms() is
 *     deliberately NOT changed: it stays on HAL_GetTick(), so scan timing,
 *     dwell and retain intervals are identical to bare-metal and do not
 *     depend on configTICK_RATE_HZ. With FreeRTOS on SysTick, put the HAL
 *     timebase on a spare TIM (CubeMX: SYS > Timebase Source), or call
 *     HAL_IncTick() from vApplicationTickHook(). The HAL Flash driver needs
 *     this tick for its own timeouts anyway.
 *   - TinyUSB stays on CFG_TUSB_OS = OPT_OS_NONE: tud_task() is only ever
 *     called from that same PLC task.
 *   - The PVD interrupt (retain snapshot) calls no RTOS API, so it may keep
 *     the highest priority.
 */

#include <stdbool.h>
#include <stdint.h>

#include "sx_os_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#if SX_OS_USE_FREERTOS

/* True once the scheduler is running (vTaskDelay is legal). Before that,
 * and in ISRs, callers must fall back to busy waiting. */
bool sx_os_is_running(void);

/* Sleep at least `ms` milliseconds through the scheduler (rounded UP to
 * whole ticks, so a non-zero request never turns into "return at once").
 * Only call when sx_os_is_running(). */
void sx_os_sleep_ms(uint32_t ms);

/* Inside a polling wait loop: give the CPU away for one tick. No-op while
 * the scheduler is not running. */
void sx_os_yield_wait(void);

#else /* bare-metal */

static inline bool sx_os_is_running(void)       { return false; }
static inline void sx_os_sleep_ms(uint32_t ms)  { (void)ms; }
static inline void sx_os_yield_wait(void)       { }

#endif

#ifdef __cplusplus
}
#endif

#endif /* SX_OS_H */