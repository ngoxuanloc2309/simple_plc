#ifndef __SX_RTC_H__
#define __SX_RTC_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * sx_rtc.h - Layer 1 (SX Driver Core)
 *
 * Chip-agnostic contract for the wall clock. Same split as sx_gpio.h /
 * sx_time.h / sx_system.h: this header is the porting boundary (pure
 * contract, no HAL_*() / CMSIS), implemented per chip one layer down
 * (platforms/stm32/stm32h5/rtc/stm32h5_rtc.c).
 *
 * The contract speaks UTC Unix epoch seconds only. Calendar conversion is
 * the driver's private business (it uses utils/epoch/splc_epoch.h); the
 * timezone offset is NOT stored here -- it belongs to the Layer 3 RTC block
 * (0x0810..0x0813), which applies it when computing the HHMM the Rule
 * Engine needs.
 *
 * "Synced" means: a Host has written a real time since the clock last lost
 * its power. The RTC on this board runs from the internal LSI with no
 * battery (no VBAT), so a power cycle always returns it to "not synced",
 * while a warm reset (REBOOT, watchdog, NVIC_SystemReset) keeps both the
 * time and the synced flag.
 */

/*
 * Sets the clock to epoch_utc_s and marks it synced.
 * Returns false (clock and synced flag unchanged) if the value is outside
 * what the hardware can hold (STM32: 2000-01-01 .. 2099-12-31) or the
 * hardware write fails.
 */
bool sx_rtc_set_epoch(uint32_t epoch_utc_s);

/*
 * Reads the current time as UTC epoch seconds into *epoch_utc_s.
 * Returns false (and leaves *epoch_utc_s untouched) if the hardware could
 * not be read. A clock that is not synced still returns true with whatever
 * it is counting from -- use sx_rtc_is_synced() to know whether to trust it.
 */
bool sx_rtc_get_epoch(uint32_t *epoch_utc_s);

/* true if sx_rtc_set_epoch() has succeeded since the clock last lost power. */
bool sx_rtc_is_synced(void);

#ifdef __cplusplus
}
#endif

#endif /* __SX_RTC_H__ */