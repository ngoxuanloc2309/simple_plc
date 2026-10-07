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

#include "plc_device.h"   /* SPLC_RtcCaps, SPLC_RtcFlags */

#ifdef __cplusplus
extern "C" {
#endif

#define PLC_RTC_ADDR_BASE   0x0810U
#define PLC_RTC_REG_COUNT   4U

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

#ifdef __cplusplus
}
#endif

#endif /* PLC_RTC_H */