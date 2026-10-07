#include "plc_rtc.h"

#include "sx_rtc.h"
#include "splc_epoch.h"
#include "logger.h"

static const char *TAG = "PLC_RTC";

SPLC_RtcCaps g_rtc_caps;   /* zero-initialised: hw_present = false */

/* What the STM32 RTC can hold: 2000-01-01 00:00:00 .. 2099-12-31 23:59:59 UTC. */
#define RTC_EPOCH_MIN   946684800UL
#define RTC_EPOCH_MAX   4102444799UL

/* Real-world UTC offsets: UTC-12:00 .. UTC+14:00. */
#define RTC_TZ_MIN_MIN  (-720)
#define RTC_TZ_MAX_MIN  (840)

static int16_t s_tz_offset_min = 0;
static bool    s_tz_known      = false;   /* written by a Host since boot */

static uint16_t status_flags_now(void)
{
    uint16_t f = 0U;
    if (sx_rtc_is_synced()) {
        f |= (uint16_t)SPLC_RTC_FLAG_SYNCED;
    }
    if (g_rtc_caps.hw_present) {
        f |= (uint16_t)SPLC_RTC_FLAG_HW_PRESENT;
    }
    /* SPLC_RTC_FLAG_BATTERY_LOW: no board reports it yet. */
    return f;
}

void plc_rtc_read(uint16_t offset, uint16_t quantity, uint16_t *registers_out)
{
    uint32_t epoch = 0U;
    if (!sx_rtc_get_epoch(&epoch)) {
        epoch = 0U;
    }

    uint16_t regs[PLC_RTC_REG_COUNT];
    regs[0] = (uint16_t)(epoch >> 16);
    regs[1] = (uint16_t)(epoch & 0xFFFFU);
    regs[2] = (uint16_t)s_tz_offset_min;
    regs[3] = status_flags_now();

    for (uint16_t i = 0; i < quantity; i++) {
        uint16_t idx = (uint16_t)(offset + i);
        registers_out[i] = (idx < PLC_RTC_REG_COUNT) ? regs[idx] : 0U;
    }
}

plc_rtc_write_result_t plc_rtc_write(uint16_t offset, uint16_t quantity,
                                     const uint16_t *registers)
{
    if (offset != 0U) {
        /* status_flags (offset 3) is read-only; a write must start at the
         * epoch so epoch and timezone always land together. */
        log_warn(TAG, "write rejected: offset=%u (must start at 0x0810)", offset);
        return PLC_RTC_WRITE_BAD_ADDRESS;
    }
    if (quantity != 3U && quantity != 4U) {
        log_warn(TAG, "write rejected: quantity=%u (need 3 or 4)", quantity);
        return PLC_RTC_WRITE_BAD_VALUE;
    }

    uint32_t epoch = ((uint32_t)registers[0] << 16) | (uint32_t)registers[1];
    int16_t  tz    = (int16_t)registers[2];
    /* registers[3] (status_flags), if present, is deliberately ignored. */

    if (epoch < RTC_EPOCH_MIN || epoch > RTC_EPOCH_MAX) {
        log_warn(TAG, "write rejected: epoch %lu outside 2000..2099", (unsigned long)epoch);
        return PLC_RTC_WRITE_BAD_VALUE;
    }
    if (tz < RTC_TZ_MIN_MIN || tz > RTC_TZ_MAX_MIN) {
        log_warn(TAG, "write rejected: tz_offset_min %d outside -720..+840", (int)tz);
        return PLC_RTC_WRITE_BAD_VALUE;
    }

    if (!sx_rtc_set_epoch(epoch)) {
        log_error(TAG, "sx_rtc_set_epoch() failed");
        return PLC_RTC_WRITE_DEVICE_FAIL;
    }

    s_tz_offset_min = tz;
    s_tz_known      = true;
    log_info(TAG, "time set: epoch=%lu tz=%d", (unsigned long)epoch, (int)tz);
    return PLC_RTC_WRITE_OK;
}

bool plc_rtc_get_local_hhmm(uint16_t *hhmm)
{
    if (!s_tz_known || !sx_rtc_is_synced()) {
        return false;
    }
    uint32_t epoch = 0U;
    if (!sx_rtc_get_epoch(&epoch)) {
        return false;
    }
    *hhmm = splc_epoch_to_hhmm(epoch, s_tz_offset_min);
    return true;
}