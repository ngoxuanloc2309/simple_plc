/*
 * plc_systag.h - Layer 3 (Services)
 *
 * System Tags (ICD SPLC-SPEC-SYSTEM-TAGS-006, Structs doc V2.0 section 3.4.1): 8 read-only
 * values the firmware refreshes once per scan cycle, readable by the Rule Engine as tag
 * 1000..1007 and by the Host over Modbus at 0x0820..0x0827 (FC03, 8 registers).
 *
 *   tag   reg     name              value
 *   1000  0x0820  SYS_FIRST_SCAN    1 in the first scan cycle after boot, 0 afterwards
 *   1001  0x0821  SYS_ON            always 1
 *   1002  0x0822  SYS_OFF           always 0
 *   1003  0x0823  SYS_CLOCK_1S      square wave, (tick_ms / 500) even -> 1  (500 ms / 500 ms)
 *   1004  0x0824  SYS_CLOCK_100MS   square wave, (tick_ms / 50) even  -> 1  (50 ms / 50 ms)
 *   1005  0x0825  SYS_RTC_HOUR      LOCAL hour 0..23  (epoch_utc + tz_offset_min*60)
 *   1006  0x0826  SYS_RTC_MINUTE    LOCAL minute 0..59
 *   1007  0x0827  SYS_COMM_OK       1 = link healthy (see plc_systag_set_comm_ok_fn())
 *
 * RTC_HOUR / RTC_MINUTE are 0 while there is no valid local time (RTC not synced since power
 * on, or timezone not written yet; same condition as plc_rtc_get_local_hhmm() returning false,
 * which also keeps TRG_TIME_WINDOW rules from firing). 0 is also a valid time, so a rule that
 * must tell the two apart needs another source.
 *
 * The block is read-only: FC06/FC16 into 0x0820..0x0827 get the normal "no write callback"
 * refusal of the Modbus block table. A rule may read these tags as trigger / guard; as an
 * action target they are refused at COMMIT (rule_table_find_bad_tag()).
 *
 * Compiled out with SPLC_SYSTAG_ENABLE = 0 (config/splc_opt.h): no code, no Modbus block.
 */
#ifndef PLC_SYSTAG_H
#define PLC_SYSTAG_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Number of Modbus registers of the block (0x0820..0x0827). */
#define PLC_SYSTAG_REG_COUNT   8U

/*
 * SYS_COMM_OK source. The host registers one function that returns true while its
 * communication link is healthy (e.g. "a Modbus master spoke to us in the last N seconds",
 * or "Wi-Fi up and MQTT connected"). Called once per scan cycle, must be cheap and must not
 * block. Without a registered function SYS_COMM_OK stays 1. Pass NULL to unregister.
 */
typedef bool (*plc_systag_comm_ok_fn)(void);
void plc_systag_set_comm_ok_fn(plc_systag_comm_ok_fn fn);

/*
 * Refresh the 8 tags. Called once at the start of every scan cycle, BEFORE the rules run
 * (also while a diag session suspends the Rule Engine, so the Modbus block keeps moving).
 * now_ms: the tick sampled at cycle start (same value rule_scan() gets). Reads the local
 * time itself through plc_rtc_get_local_hhmm().
 */
void plc_systag_scan(uint32_t now_ms);

/* Same as plc_systag_scan() with the time given (PC tests, hosts that already hold HHMM).
 * hhmm_valid=false -> RTC_HOUR/MINUTE = 0. hhmm = hour*100 + minute. */
void plc_systag_update(uint32_t now_ms, bool hhmm_valid, uint16_t hhmm);

/* Modbus read callback of 0x0820..0x0827 (offset relative to 0x0820), same shape as
 * plc_rtc_read(); registers past the block read as 0. */
void plc_systag_read(uint16_t offset, uint16_t quantity, uint16_t *registers_out);

#ifdef __cplusplus
}
#endif

#endif /* PLC_SYSTAG_H */