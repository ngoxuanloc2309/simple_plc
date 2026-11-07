#include "splc_rtc_port.h"
#include "splc_opt.h"

#if SPLC_RTC_EXTERNAL

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "splc_log.h"
#include "plc_rtc.h"        /* plc_rtc_register(), g_rtc_caps */
#include "splc_epoch.h"
#include "rtc.h"            /* CubeMX-generated: hrtc */

static const char *TAG = "RTC_PORT";

/* One consistent read: the HAL needs GetTime() before GetDate(); a rollover between the two calls
 * is caught by reading the time again and comparing. */
static bool rtc_read(splc_datetime_t *dt)
{
    RTC_TimeTypeDef t1, t2;
    RTC_DateTypeDef d;
    if (hrtc.Instance == NULL) {
        return false;
    }
    if (HAL_RTC_GetTime(&hrtc, &t1, RTC_FORMAT_BIN) != HAL_OK ||
        HAL_RTC_GetDate(&hrtc, &d, RTC_FORMAT_BIN) != HAL_OK ||
        HAL_RTC_GetTime(&hrtc, &t2, RTC_FORMAT_BIN) != HAL_OK) {
        return false;
    }
    if (t1.Seconds != t2.Seconds) {
        if (HAL_RTC_GetDate(&hrtc, &d, RTC_FORMAT_BIN) != HAL_OK) {
            return false;
        }
    }
    dt->year    = (uint16_t)(2000U + d.Year);
    dt->month   = d.Month;
    dt->day     = d.Date;
    dt->hour    = t2.Hours;
    dt->minute  = t2.Minutes;
    dt->second  = t2.Seconds;
    dt->weekday = d.WeekDay;
    return true;
}

static bool rtc_get_epoch(void *ctx, uint32_t *epoch_utc_s)
{
    (void)ctx;
    splc_datetime_t dt;
    return rtc_read(&dt) && splc_datetime_to_epoch(&dt, epoch_utc_s);
}

static bool rtc_set_epoch(void *ctx, uint32_t epoch_utc_s)
{
    (void)ctx;
    splc_datetime_t dt;
    if (hrtc.Instance == NULL || !splc_epoch_to_datetime(epoch_utc_s, &dt) || dt.year < 2000U) {
        return false;
    }
    RTC_TimeTypeDef t;
    RTC_DateTypeDef d;
    memset(&t, 0, sizeof t);
    memset(&d, 0, sizeof d);
    t.Hours   = dt.hour;
    t.Minutes = dt.minute;
    t.Seconds = dt.second;
    d.Year    = (uint8_t)(dt.year - 2000U);
    d.Month   = dt.month;
    d.Date    = dt.day;
    d.WeekDay = dt.weekday;                 /* 1 = Monday .. 7 = Sunday, same as the HAL */
    if (HAL_RTC_SetTime(&hrtc, &t, RTC_FORMAT_BIN) != HAL_OK ||
        HAL_RTC_SetDate(&hrtc, &d, RTC_FORMAT_BIN) != HAL_OK) {
        log_error(TAG, "HAL_RTC_Set* failed");
        return false;
    }
    return true;
}

void splc_rtc_port_init(void)
{
    const plc_rtc_ops_t ops = {
        .ctx       = NULL,
        .get_epoch = rtc_get_epoch,
        .set_epoch = rtc_set_epoch,
        .is_synced = NULL,
    };
    if (!plc_rtc_register(&ops)) {
        log_error(TAG, "plc_rtc_register failed");
        return;
    }
    g_rtc_caps.hw_present = true;
}

#else

void splc_rtc_port_init(void) {}

#endif