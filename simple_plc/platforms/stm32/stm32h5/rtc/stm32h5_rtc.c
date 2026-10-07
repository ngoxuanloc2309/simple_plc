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

/*
 * Keeping the time across a warm reset WITHOUT touching CubeMX files.
 *
 * Problem: the generated MX_RTC_Init() (Core/Src/rtc.c, called from main())
 * ends with an unconditional HAL_RTC_SetTime()/HAL_RTC_SetDate() of
 * 2000-01-01 00:00:00. On a warm reset (REBOOT, watchdog) the RTC itself kept
 * counting, and that call would overwrite the time Studio had given us.
 *
 * Fix: the linker option --wrap=HAL_RTC_SetTime / --wrap=HAL_RTC_SetDate
 * (set in simple_plc/CMakeLists.txt) redirects every call to those two HAL
 * functions to the __wrap_ functions below. They let the call through only
 * when it comes from this driver, or when the clock has never been synced
 * (first power-up: writing the CubeMX default is harmless). While the clock
 * is synced, the CubeMX default write at boot is silently skipped.
 *
 * Why this and not an edit in rtc.c: nothing generated is modified, so
 * regenerating the project or moving to other hardware cannot lose it. If
 * the RTC is ever removed from CubeMX the wrappers simply have no caller.
 *
 * If the build fails with "undefined reference to __real_HAL_RTC_SetTime",
 * the --wrap link options were dropped from CMake -- restore them (failing
 * loudly is intentional: without them the time would silently be lost on
 * every REBOOT).
 */
extern HAL_StatusTypeDef __real_HAL_RTC_SetTime(RTC_HandleTypeDef *h, RTC_TimeTypeDef *t, uint32_t fmt);
extern HAL_StatusTypeDef __real_HAL_RTC_SetDate(RTC_HandleTypeDef *h, RTC_DateTypeDef *d, uint32_t fmt);

static bool s_driver_write = false;   /* true only inside sx_rtc_set_epoch() */

HAL_StatusTypeDef __wrap_HAL_RTC_SetTime(RTC_HandleTypeDef *h, RTC_TimeTypeDef *t, uint32_t fmt)
{
    if (!s_driver_write && sx_rtc_is_synced()) {
        return HAL_OK;   /* keep the running, synced time */
    }
    return __real_HAL_RTC_SetTime(h, t, fmt);
}

HAL_StatusTypeDef __wrap_HAL_RTC_SetDate(RTC_HandleTypeDef *h, RTC_DateTypeDef *d, uint32_t fmt)
{
    if (!s_driver_write && sx_rtc_is_synced()) {
        return HAL_OK;
    }
    return __real_HAL_RTC_SetDate(h, d, fmt);
}

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

    s_driver_write = true;   /* let our own calls through the __wrap_ filter above */
    const bool ok = (HAL_RTC_SetTime(&hrtc, &t, RTC_FORMAT_BIN) == HAL_OK) &&
                    (HAL_RTC_SetDate(&hrtc, &d, RTC_FORMAT_BIN) == HAL_OK);
    s_driver_write = false;
    if (!ok) {
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