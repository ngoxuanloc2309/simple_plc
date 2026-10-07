#ifndef SPLC_EPOCH_H
#define SPLC_EPOCH_H

/*
 * splc_epoch.h - Layer U (Utils)
 *
 * Pure calendar arithmetic, no hardware and no libc time functions (no
 * time.h, no timezone database, no heap): Unix epoch <-> calendar date/time,
 * and the HHMM value the Rule Engine's TRG_TIME_WINDOW compares against.
 *
 * Kept in Layer U so it builds and unit-tests on a plain PC, and so every
 * chip's RTC driver (Layer 0) shares one conversion instead of each port
 * re-implementing it.
 *
 * Range: 1970-01-01 .. 2105-12-31 (uint32_t seconds). The STM32 RTC itself
 * only holds years 2000..2099, so the Layer 0 driver narrows this further.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t year;     /* full year, e.g. 2026 */
    uint8_t  month;    /* 1..12 */
    uint8_t  day;      /* 1..31 */
    uint8_t  hour;     /* 0..23 */
    uint8_t  minute;   /* 0..59 */
    uint8_t  second;   /* 0..59 */
    uint8_t  weekday;  /* 1=Monday .. 7=Sunday (same numbering as the STM32 RTC) */
} splc_datetime_t;

/* Epoch seconds (UTC, since 1970-01-01 00:00:00) -> calendar fields.
 * Returns false only if dt == NULL. */
bool splc_epoch_to_datetime(uint32_t epoch_s, splc_datetime_t *dt);

/* Calendar fields -> epoch seconds. weekday is ignored on input.
 * Returns false on NULL, an out-of-range field (month 1..12, day valid for
 * that month/year, hour<24, minute<60, second<60), or a result outside
 * 0..UINT32_MAX. */
bool splc_datetime_to_epoch(const splc_datetime_t *dt, uint32_t *epoch_s);

/*
 * Local time-of-day as HHMM (e.g. 07:05 -> 705, 23:59 -> 2359), from a UTC
 * epoch and a timezone offset in minutes (+420 = UTC+7). Works for negative
 * offsets and for epochs close to 0 (no unsigned underflow).
 *
 *   local_epoch    = epoch_utc_s + tz_offset_min * 60
 *   seconds_of_day = local_epoch mod 86400   (always 0..86399)
 *   hhmm           = (seconds_of_day / 3600) * 100 + (seconds_of_day % 3600) / 60
 *
 * Same formula as docs/SimplePLC_App_MCU_Structs_v2.0 section 7.
 */
uint16_t splc_epoch_to_hhmm(uint32_t epoch_utc_s, int16_t tz_offset_min);

#ifdef __cplusplus
}
#endif

#endif /* SPLC_EPOCH_H */