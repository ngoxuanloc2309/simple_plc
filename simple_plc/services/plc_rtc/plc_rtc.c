#include "plc_rtc.h"

#include "splc_epoch.h"
#include "splc_log.h"

static const char *TAG = "PLC_RTC";

/* What the STM32 RTC can hold: 2000-01-01 00:00:00 .. 2099-12-31 23:59:59 UTC. */
#define RTC_EPOCH_MIN   946684800UL
#define RTC_EPOCH_MAX   4102444799UL

/* Real-world UTC offsets: UTC-12:00 .. UTC+14:00. */
#define RTC_TZ_MIN_MIN  (-720)
#define RTC_TZ_MAX_MIN  (840)

static int16_t s_tz_offset_min = 0;
static bool    s_tz_known      = false;   /* written by a Host since boot, or restored by tz_load */

/* ----------------------------------------------------------------------
 * Clock backend: hw_get_epoch() / hw_set_epoch() / hw_is_synced().
 *   SPLC_RTC_EXTERNAL = 0: the Layer 1 contract sx_rtc_*() (built-in STM32H5
 *                          driver or the host's own), mapped 1:1 by macros so
 *                          the object code is the one this file always had.
 *   SPLC_RTC_EXTERNAL = 1: hooks registered with plc_rtc_register(), behind a
 *                          small cache so a slow bus never stalls a scan.
 * -------------------------------------------------------------------- */
#if !SPLC_RTC_EXTERNAL
#include "sx_rtc.h"
#define hw_get_epoch(p)   sx_rtc_get_epoch(p)
#define hw_set_epoch(e)   sx_rtc_set_epoch(e)
#define hw_is_synced()    sx_rtc_is_synced()
#define HW_SET_FAIL_MSG   "sx_rtc_set_epoch() failed"
#else /* SPLC_RTC_EXTERNAL */
#include <stddef.h>
#include "sx_time.h"

#define HW_SET_FAIL_MSG   "set_epoch hook failed"

static plc_rtc_ops_t s_ops;
static bool     s_ops_set;
static bool     s_host_set;        /* a Host wrote the time since boot (used when no is_synced hook) */
static bool     s_attempted;       /* a refresh has been tried since registration / last write */
static uint32_t s_last_read_ms;    /* tick of the last refresh attempt */
static bool     s_cache_ok;        /* last get_epoch() succeeded */
static uint32_t s_cache_epoch;     /* epoch at s_cache_ms */
static uint32_t s_cache_ms;
static bool     s_cache_synced;
static bool     s_tz_load_pending; /* tz_load() has not given a final answer yet */
static bool     s_tz_tried;        /* tz_load() was called at least once */
static uint32_t s_tz_last_try_ms;  /* tick of the last tz_load() call */
static bool     s_tz_stored_valid; /* the product's store is known to hold s_tz_stored */
static int16_t  s_tz_stored;

/* Asks the product for the stored timezone offset (if it has a tz_load hook) until it gives a
 * final answer. A Host that already wrote the offset wins: nothing is restored over it. */
static void tz_try_restore(void)
{
    if (!s_tz_load_pending || !s_ops_set || s_ops.tz_load == NULL) {
        return;
    }
    if (s_tz_known) {
        s_tz_load_pending = false;
        return;
    }
    /* Asked at most once per SPLC_RTC_EXT_READ_PERIOD_MS: this runs from the scan path, so a store
     * that cannot be read must not be hammered every 10 ms. */
    const uint32_t now = sx_get_tick_ms();
    if (s_tz_tried && (uint32_t)(now - s_tz_last_try_ms) < (uint32_t)SPLC_RTC_EXT_READ_PERIOD_MS) {
        return;
    }
    s_tz_tried       = true;
    s_tz_last_try_ms = now;
    int16_t tz = 0;
    const int r = s_ops.tz_load(s_ops.ctx, &tz);
    if (r < 0) {
        return;                                   /* store not readable yet: ask again later */
    }
    s_tz_load_pending = false;
    if (r == 0) {
        log_info(TAG, "no stored time zone, waiting for the Host to write it");
        return;
    }
    if (tz < RTC_TZ_MIN_MIN || tz > RTC_TZ_MAX_MIN) {
        log_warn(TAG, "stored time zone %d out of range, ignored", (int)tz);
        return;
    }
    s_tz_offset_min   = tz;
    s_tz_known        = true;
    s_tz_stored       = tz;
    s_tz_stored_valid = true;
    log_info(TAG, "time zone restored from storage: tz=%d", (int)tz);
}

/* After a Host write: store the offset when it differs from what the store holds. */
static void tz_persist(int16_t tz)
{
    if (!s_ops_set || s_ops.tz_save == NULL) {
        return;
    }
    if (s_tz_stored_valid && s_tz_stored == tz) {
        return;
    }
    if (s_ops.tz_save(s_ops.ctx, tz)) {
        s_tz_stored       = tz;
        s_tz_stored_valid = true;
        log_info(TAG, "time zone stored: tz=%d", (int)tz);
    } else {
        log_warn(TAG, "time zone NOT stored (tz_save failed), it is lost at the next reset");
    }
}

bool plc_rtc_register(const plc_rtc_ops_t *ops)
{
    if (ops == NULL || ops->get_epoch == NULL || ops->set_epoch == NULL) {
        log_error(TAG, "register rejected: get_epoch and set_epoch are required");
        return false;
    }
    s_ops          = *ops;
    s_ops_set      = true;
    s_host_set     = false;
    s_attempted    = false;
    s_cache_ok     = false;
    s_cache_synced = false;
    s_tz_load_pending = (ops->tz_load != NULL);
    s_tz_stored_valid = false;
    s_tz_tried        = false;
    tz_try_restore();
    return true;
}

/* Calls the hooks if the last attempt is older than SPLC_RTC_EXT_READ_PERIOD_MS
 * (a failed attempt waits the same time, so a dead chip is not hammered). */
static void ext_refresh(void)
{
    if (!s_ops_set) {
        return;
    }
    uint32_t now = sx_get_tick_ms();
    if (s_attempted &&
        (uint32_t)(now - s_last_read_ms) < (uint32_t)SPLC_RTC_EXT_READ_PERIOD_MS) {
        return;
    }
    s_attempted    = true;
    s_last_read_ms = now;

    uint32_t epoch = 0U;
    if (s_ops.get_epoch(s_ops.ctx, &epoch)) {
        s_cache_epoch = epoch;
        s_cache_ms    = now;
        s_cache_ok    = true;
    } else {
        s_cache_ok = false;
    }
    if (s_ops.is_synced != NULL) {
        s_cache_synced = s_ops.is_synced(s_ops.ctx);
    }
    tz_try_restore();
}

static bool hw_get_epoch(uint32_t *epoch_utc_s)
{
    ext_refresh();
    if (!s_ops_set || !s_cache_ok) {
        return false;
    }
    *epoch_utc_s = s_cache_epoch +
                   (uint32_t)((uint32_t)(sx_get_tick_ms() - s_cache_ms) / 1000U);
    return true;
}

static bool hw_set_epoch(uint32_t epoch_utc_s)
{
    if (!s_ops_set) {
        return false;
    }
    if (!s_ops.set_epoch(s_ops.ctx, epoch_utc_s)) {
        return false;
    }
    s_host_set  = true;
    s_attempted = false;    /* read the chip back on the next call */
    return true;
}

static bool hw_is_synced(void)
{
    ext_refresh();
    if (!s_ops_set || !s_cache_ok) {
        return false;    /* cannot read the chip: do not claim a trustworthy time */
    }
    return (s_ops.is_synced != NULL) ? s_cache_synced : s_host_set;
}
#endif /* SPLC_RTC_EXTERNAL */

#if SPLC_RTC_EXTERNAL
/* The two local-time getters return early while the offset is unknown, i.e. before they touch the
 * clock; they are exactly where a restore that could not finish at boot has to be retried. */
#define TZ_RESTORE_POLL()   tz_try_restore()
#else
#define TZ_RESTORE_POLL()   ((void)0)
#endif

SPLC_RtcCaps g_rtc_caps;   /* zero-initialised: hw_present = false */


static uint16_t status_flags_now(void)
{
    uint16_t f = 0U;
    if (hw_is_synced()) {
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
    if (!hw_get_epoch(&epoch)) {
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

    if (!hw_set_epoch(epoch)) {
        log_error(TAG, HW_SET_FAIL_MSG);
        return PLC_RTC_WRITE_DEVICE_FAIL;
    }

    s_tz_offset_min = tz;
    s_tz_known      = true;
    log_info(TAG, "time set: epoch=%lu tz=%d", (unsigned long)epoch, (int)tz);
#if SPLC_RTC_EXTERNAL
    tz_persist(tz);
#endif
    return PLC_RTC_WRITE_OK;
}

bool plc_rtc_get_local_hhmm(uint16_t *hhmm)
{
    TZ_RESTORE_POLL();
    if (!s_tz_known || !hw_is_synced()) {
        return false;
    }
    uint32_t epoch = 0U;
    if (!hw_get_epoch(&epoch)) {
        return false;
    }
    *hhmm = splc_epoch_to_hhmm(epoch, s_tz_offset_min);
    return true;
}

bool plc_rtc_get_local_datetime(splc_datetime_t *dt)
{
    uint32_t epoch = 0U;
    TZ_RESTORE_POLL();
    if (dt == NULL || !s_tz_known || !hw_is_synced() || !hw_get_epoch(&epoch)) {
        return false;
    }
    /* epoch + tz stays inside uint32 for 2000..2099 and |tz| <= 14 h */
    const int64_t local = (int64_t)epoch + (int64_t)s_tz_offset_min * 60;
    if (local < 0 || local > (int64_t)UINT32_MAX) {
        return false;
    }
    return splc_epoch_to_datetime((uint32_t)local, dt);
}

bool plc_rtc_get_epoch(uint32_t *epoch)
{
    uint32_t now;
    if (epoch == NULL || !hw_is_synced() || !hw_get_epoch(&now)) {
        return false;
    }
    *epoch = now;
    return true;
}