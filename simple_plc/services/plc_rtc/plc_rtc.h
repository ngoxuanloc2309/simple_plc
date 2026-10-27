#ifndef PLC_RTC_H
#define PLC_RTC_H

/*
 * plc_rtc.h - Layer 3 (PLC Application Services)
 *
 * The RTC register block (0x0810..0x0813, Wire Profile V2.0, Structs doc
 * section 3.4) and the local time of day the Rule Engine's TRG_TIME_WINDOW
 * needs (Structs doc section 7).
 *
 * Wire layout, 4 registers:
 *   0x0810..0x0811  epoch_utc_s   uint32, High Word first   Host writes
 *   0x0812          tz_offset_min int16  (+420 = UTC+7)     Host writes
 *   0x0813          status_flags  uint16                    READ-ONLY, board-reported
 *
 * status_flags is NEVER taken from the Host. It is computed on every read:
 *   SYNCED      <- sx_rtc_is_synced()   (a Host has set the time)
 *                  [SPLC_RTC_EXTERNAL = 1: the registered is_synced hook, or
 *                  "a Host has set the time since boot" when there is none]
 *   HW_PRESENT  <- g_rtc_caps.hw_present (set by the board's own init)
 *   BATTERY_LOW <- always 0 (no board reports it yet)
 * so a Host that still sends a 4th register has it ignored.
 *
 * The timezone offset lives in RAM only (never Flash) and applies at the
 * moment the Host writes it. Until it has been written since boot,
 * plc_rtc_get_local_hhmm() reports "no valid time" even if the RTC itself
 * kept its time across a warm reset: without the offset the local hour
 * would silently be UTC.
 *
 * This file is transport-agnostic: it does not include nanomodbus.h.
 * plc_modbus_cfg.c maps plc_rtc_write()'s result onto Modbus exceptions.
 */

#include <stdbool.h>
#include <stdint.h>

#include "splc_opt.h"     /* SPLC_RTC_EXTERNAL */
#include "plc_device.h"   /* SPLC_RtcCaps, SPLC_RtcFlags */

#ifdef __cplusplus
extern "C" {
#endif

#define PLC_RTC_ADDR_BASE   0x0810U
#define PLC_RTC_REG_COUNT   4U

#if SPLC_RTC_EXTERNAL
/*
 * External RTC chip (SPLC_RTC_EXTERNAL = 1) -- registered hooks.
 *
 * SimplePLC has no driver for the chip. The product owns it (I2C/SPI, its own
 * calendar maths) and hands the library two functions, exactly like
 * splc_link_register() hands it a byte pipe. The contract speaks UTC Unix
 * epoch seconds only; the timezone offset stays inside SimplePLC.
 *
 *   get_epoch   (required) read the chip, *epoch_utc_s = UTC epoch seconds.
 *               Return false if the chip could not be read.
 *   set_epoch   (required) write the chip from UTC epoch seconds (the library
 *               has already checked 2000-01-01 .. 2099-12-31). Return false on
 *               failure; the library then answers the Host with a device error.
 *   is_synced   (optional) true when the chip holds a trustworthy time -- e.g.
 *               its oscillator-stop / power-loss flag is clear. NULL = the
 *               library reports "synced" only after a Host has written the
 *               time since this boot (a battery-backed chip should supply the
 *               hook, otherwise Time Window rules stay quiet after every
 *               power cycle until Studio writes the time again).
 *   ctx         passed back as the first argument of every hook.
 *
 * Rules for the hooks: they run in the ENGINE context (the one that calls
 * plc_engine_poll()), never from an ISR; keep them short (a 7-byte I2C
 * transfer is fine); do not call any plc_* / splc_* function from inside.
 * The library calls get_epoch / is_synced at most every
 * SPLC_RTC_EXT_READ_PERIOD_MS (default 1000 ms) and counts the time in
 * between from sx_get_tick_ms(), so the 10 ms scan never waits on the bus.
 * set_epoch is called once per Host write, and the next read-back follows
 * immediately.
 *
 * Set g_rtc_caps.hw_present = true in the board init as usual: it only feeds
 * the HW_PRESENT bit of status_flags.
 */
typedef struct {
    void *ctx;
    bool (*get_epoch)(void *ctx, uint32_t *epoch_utc_s);
    bool (*set_epoch)(void *ctx, uint32_t epoch_utc_s);
    bool (*is_synced)(void *ctx);          /* optional, may be NULL */
} plc_rtc_ops_t;

/*
 * Registers (or replaces) the hooks. The struct is COPIED. Call it before
 * plc_engine_init() (or at least before the first scan), from the engine
 * context. Returns false and changes nothing if ops is NULL or get_epoch /
 * set_epoch is NULL. Until it succeeds the clock reads "unavailable": time
 * reads fail, a Host write is answered with a device error, SYNCED = 0.
 */
bool plc_rtc_register(const plc_rtc_ops_t *ops);
#endif /* SPLC_RTC_EXTERNAL */

/*
 * Board clock capabilities, written ONCE at boot by the board's init
 * (board_<sku>.c), same pattern as g_device_resource_info. Left
 * zero-initialised (hw_present = false) if a board never sets it.
 */
extern SPLC_RtcCaps g_rtc_caps;

/*
 * FC03 read of `quantity` registers starting at `offset` (0..3) inside the
 * block. Always succeeds. The epoch is whatever the RTC is counting from;
 * read SYNCED in status_flags to know whether to trust it.
 */
void plc_rtc_read(uint16_t offset, uint16_t quantity, uint16_t *registers_out);

typedef enum {
    PLC_RTC_WRITE_OK = 0,
    PLC_RTC_WRITE_BAD_ADDRESS,   /* -> Modbus 0x02 */
    PLC_RTC_WRITE_BAD_VALUE,     /* -> Modbus 0x03 */
    PLC_RTC_WRITE_DEVICE_FAIL    /* -> Modbus 0x04 */
} plc_rtc_write_result_t;

/*
 * FC16 write. Accepted forms, both starting at 0x0810 (offset 0):
 *   quantity 4 : epoch, tz, status_flags (the 4th register is ignored)
 *   quantity 3 : epoch, tz
 * Anything else: BAD_ADDRESS if it does not start at offset 0 (status_flags
 * is read-only, nothing else is writable), BAD_VALUE for any other
 * quantity (FC06 arrives here as quantity 1 and is rejected this way).
 *
 * All-or-nothing: epoch must be inside 2000-01-01 .. 2099-12-31 UTC (what
 * the STM32 RTC can hold) and tz_offset_min inside -720..+840, otherwise
 * BAD_VALUE and NOTHING is changed (neither clock nor offset). If the
 * hardware write itself fails: DEVICE_FAIL, offset unchanged.
 */
plc_rtc_write_result_t plc_rtc_write(uint16_t offset, uint16_t quantity,
                                     const uint16_t *registers);

/*
 * Local time of day as HHMM (0..2359) for the Rule Engine. Returns false
 * (and leaves *hhmm untouched) unless the clock is SYNCED, the timezone
 * offset has been written since boot, and the RTC could be read.
 * Cheap enough to call every scan cycle.
 */
bool plc_rtc_get_local_hhmm(uint16_t *hhmm);

/*
 * Current UTC epoch seconds (for the MQTT {ts} token). Returns false (and
 * leaves *epoch untouched) unless the clock is SYNCED and could be read, so
 * a caller never publishes the RTC's free-running count from 2000-01-01 as
 * if it were real time. Does not need the timezone offset.
 */
bool plc_rtc_get_epoch(uint32_t *epoch);

#ifdef __cplusplus
}
#endif

#endif /* PLC_RTC_H */