/*
 * plc_systag.c - Layer 3 (Services). See plc_systag.h.
 */
#include "plc_systag.h"

#include <stddef.h>

#include "plc_tag.h"
#include "plc_rtc.h"

enum {
    SYS_FIRST_SCAN = 0,   /* tag 1000 */
    SYS_ON,               /* 1001 */
    SYS_OFF,              /* 1002 */
    SYS_CLOCK_1S,         /* 1003 */
    SYS_CLOCK_100MS,      /* 1004 */
    SYS_RTC_HOUR,         /* 1005 */
    SYS_RTC_MINUTE,       /* 1006 */
    SYS_COMM_OK           /* 1007 */
};

static bool                  s_first_scan_done = false;
static plc_systag_comm_ok_fn s_comm_ok_fn      = NULL;

void plc_systag_set_comm_ok_fn(plc_systag_comm_ok_fn fn)
{
    s_comm_ok_fn = fn;
}

void plc_systag_update(uint32_t now_ms, bool hhmm_valid, uint16_t hhmm)
{
    tag_system_set(SYS_FIRST_SCAN, s_first_scan_done ? 0 : 1);
    s_first_scan_done = true;

    tag_system_set(SYS_ON,  1);
    tag_system_set(SYS_OFF, 0);

    /* Even half-period -> 1: the very first 500 ms (50 ms) of the tick counter are high. */
    tag_system_set(SYS_CLOCK_1S,    (((now_ms / 500U) % 2U) == 0U) ? 1 : 0);
    tag_system_set(SYS_CLOCK_100MS, (((now_ms /  50U) % 2U) == 0U) ? 1 : 0);

    int32_t hour   = 0;
    int32_t minute = 0;
    if (hhmm_valid) {
        hour   = (int32_t)(hhmm / 100U);
        minute = (int32_t)(hhmm % 100U);
        if (hour > 23 || minute > 59) {   /* cannot happen with splc_epoch_to_hhmm(); belt and braces */
            hour = 0;
            minute = 0;
        }
    }
    tag_system_set(SYS_RTC_HOUR,   hour);
    tag_system_set(SYS_RTC_MINUTE, minute);

    tag_system_set(SYS_COMM_OK, (s_comm_ok_fn == NULL || s_comm_ok_fn()) ? 1 : 0);
}

void plc_systag_scan(uint32_t now_ms)
{
    uint16_t hhmm = 0U;
    const bool valid = plc_rtc_get_local_hhmm(&hhmm);
    plc_systag_update(now_ms, valid, hhmm);
}

void plc_systag_read(uint16_t offset, uint16_t quantity, uint16_t *registers_out)
{
    for (uint16_t i = 0; i < quantity; i++) {
        const uint16_t idx = (uint16_t)(offset + i);
        registers_out[i] = (idx < PLC_SYSTAG_REG_COUNT) ? (uint16_t)tag_system_get(idx) : 0U;
    }
}