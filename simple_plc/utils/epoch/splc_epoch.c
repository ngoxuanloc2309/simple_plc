#include "splc_epoch.h"

#include <stddef.h>

/*
 * Civil-date <-> day-count conversion (H. Hinnant's days_from_civil /
 * civil_from_days), integer only. Day 0 = 1970-01-01.
 */

static bool is_leap(uint32_t y)
{
    return ((y % 4U) == 0U && (y % 100U) != 0U) || ((y % 400U) == 0U);
}

static uint8_t days_in_month(uint32_t y, uint8_t m)
{
    static const uint8_t dim[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2U && is_leap(y)) {
        return 29U;
    }
    return dim[m - 1U];
}

static int64_t days_from_civil(int64_t y, uint32_t m, uint32_t d)
{
    y -= (m <= 2U) ? 1 : 0;
    const int64_t  era = (y >= 0 ? y : y - 399) / 400;
    const uint32_t yoe = (uint32_t)(y - era * 400);                              /* [0, 399] */
    const uint32_t doy = (153U * (m + (m > 2U ? (uint32_t)-3 : 9U)) + 2U) / 5U + d - 1U; /* [0, 365] */
    const uint32_t doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;               /* [0, 146096] */
    return era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, uint32_t *y, uint8_t *m, uint8_t *d)
{
    z += 719468;
    const int64_t  era = (z >= 0 ? z : z - 146096) / 146097;
    const uint32_t doe = (uint32_t)(z - era * 146097);                               /* [0, 146096] */
    const uint32_t yoe = (doe - doe / 1460U + doe / 36524U - doe / 146096U) / 365U;  /* [0, 399] */
    const int64_t  yy  = (int64_t)yoe + era * 400;
    const uint32_t doy = doe - (365U * yoe + yoe / 4U - yoe / 100U);                 /* [0, 365] */
    const uint32_t mp  = (5U * doy + 2U) / 153U;                                     /* [0, 11] */
    *d = (uint8_t)(doy - (153U * mp + 2U) / 5U + 1U);
    *m = (uint8_t)(mp < 10U ? mp + 3U : mp - 9U);
    *y = (uint32_t)(yy + ((*m <= 2U) ? 1 : 0));
}

bool splc_epoch_to_datetime(uint32_t epoch_s, splc_datetime_t *dt)
{
    if (dt == NULL) {
        return false;
    }
    const uint32_t days = epoch_s / 86400U;
    const uint32_t sod  = epoch_s % 86400U;
    uint32_t y;
    uint8_t  m, d;

    civil_from_days((int64_t)days, &y, &m, &d);

    dt->year   = (uint16_t)y;
    dt->month  = m;
    dt->day    = d;
    dt->hour   = (uint8_t)(sod / 3600U);
    dt->minute = (uint8_t)((sod % 3600U) / 60U);
    dt->second = (uint8_t)(sod % 60U);

    /* 1970-01-01 was a Thursday. (days + 4) % 7: 0=Sunday .. 6=Saturday. */
    const uint32_t w = (days + 4U) % 7U;
    dt->weekday = (uint8_t)(w == 0U ? 7U : w);
    return true;
}

bool splc_datetime_to_epoch(const splc_datetime_t *dt, uint32_t *epoch_s)
{
    if (dt == NULL || epoch_s == NULL) {
        return false;
    }
    if (dt->month < 1U || dt->month > 12U || dt->day < 1U ||
        dt->hour > 23U || dt->minute > 59U || dt->second > 59U) {
        return false;
    }
    if (dt->day > days_in_month(dt->year, dt->month)) {
        return false;
    }

    const int64_t days = days_from_civil((int64_t)dt->year, dt->month, dt->day);
    const int64_t secs = days * 86400 + (int64_t)dt->hour * 3600 +
                         (int64_t)dt->minute * 60 + (int64_t)dt->second;
    if (secs < 0 || secs > (int64_t)UINT32_MAX) {
        return false;
    }
    *epoch_s = (uint32_t)secs;
    return true;
}

uint16_t splc_epoch_to_hhmm(uint32_t epoch_utc_s, int16_t tz_offset_min)
{
    int64_t local = (int64_t)epoch_utc_s + (int64_t)tz_offset_min * 60;
    int64_t sod   = local % 86400;
    if (sod < 0) {
        sod += 86400;
    }
    return (uint16_t)((sod / 3600) * 100 + (sod % 3600) / 60);
}