#include "stm32h5_rtc.h"

#if STM32H5_PLATFORM

#include "rtc.h"          /* CubeMX: extern RTC_HandleTypeDef hrtc; MX_RTC_Init() */
#include "splc_epoch.h"   /* Layer U: epoch <-> calendar */

/*
 * The RTC peripheral itself (clock source LSI, prescalers 127/249 -> 1 Hz,
 * calendar, 24 h) is configured by CubeMX in Core/Src/rtc.c and started by
 * MX_RTC_Init() from main(). This file only reads and writes the calendar
 * and the synced marker; it never re-initialises the peripheral.
 *
 * STM32 RTC year register holds 0..99 = years 2000..2099, in either BIN or
 * BCD (HAL does the conversion for RTC_FORMAT_BIN).
 */
#define RTC_YEAR_BASE   2000U
#define RTC_YEAR_MAX    2099U

bool sx_rtc_is_synced(void)
{
    return HAL_RTCEx_BKUPRead(&hrtc, STM32H5_RTC_SYNC_BKP_REG) == STM32H5_RTC_SYNC_MAGIC;
}

bool sx_rtc_set_epoch(uint32_t epoch_utc_s)
{
    splc_datetime_t dt;
    if (!splc_epoch_to_datetime(epoch_utc_s, &dt)) {
        return false;
    }
    if (dt.year < RTC_YEAR_BASE || dt.year > RTC_YEAR_MAX) {
        return false;
    }

    /* Writing the calendar / backup registers needs the backup-domain
     * protection (PWR_DBPCR.DBP) off. Enabling is idempotent. */
    HAL_PWR_EnableBkUpAccess();

    RTC_TimeTypeDef t = {0};
    RTC_DateTypeDef d = {0};

    t.Hours          = dt.hour;
    t.Minutes        = dt.minute;
    t.Seconds        = dt.second;
    t.TimeFormat     = RTC_HOURFORMAT12_AM;       /* ignored in 24 h mode */
    t.SubSeconds     = 0;
    t.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
    t.StoreOperation = RTC_STOREOPERATION_RESET;

    d.WeekDay = dt.weekday;                       /* 1=Mon .. 7=Sun, same as RTC_WEEKDAY_* */
    d.Month   = dt.month;
    d.Date    = dt.day;
    d.Year    = (uint8_t)(dt.year - RTC_YEAR_BASE);

    if (HAL_RTC_SetTime(&hrtc, &t, RTC_FORMAT_BIN) != HAL_OK) {
        return false;
    }
    if (HAL_RTC_SetDate(&hrtc, &d, RTC_FORMAT_BIN) != HAL_OK) {
        return false;
    }

    /* Marker last: "synced" must never be true for a half-written clock. */
    HAL_RTCEx_BKUPWrite(&hrtc, STM32H5_RTC_SYNC_BKP_REG, STM32H5_RTC_SYNC_MAGIC);
    return true;
}

bool sx_rtc_get_epoch(uint32_t *epoch_utc_s)
{
    if (epoch_utc_s == NULL) {
        return false;
    }

    RTC_TimeTypeDef t = {0};
    RTC_DateTypeDef d = {0};

    /* HAL contract: GetTime MUST be followed by GetDate. It unlocks the
     * shadow registers, so skipping GetDate freezes the next GetTime. */
    if (HAL_RTC_GetTime(&hrtc, &t, RTC_FORMAT_BIN) != HAL_OK) {
        return false;
    }
    if (HAL_RTC_GetDate(&hrtc, &d, RTC_FORMAT_BIN) != HAL_OK) {
        return false;
    }

    splc_datetime_t dt;
    dt.year    = (uint16_t)(RTC_YEAR_BASE + d.Year);
    dt.month   = d.Month;
    dt.day     = d.Date;
    dt.hour    = t.Hours;
    dt.minute  = t.Minutes;
    dt.second  = t.Seconds;
    dt.weekday = d.WeekDay;

    uint32_t epoch;
    if (!splc_datetime_to_epoch(&dt, &epoch)) {
        return false;
    }
    *epoch_utc_s = epoch;
    return true;
}

#endif // STM32H5_PLATFORM