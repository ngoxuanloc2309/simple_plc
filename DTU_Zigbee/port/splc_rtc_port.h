#ifndef SPLC_RTC_PORT_H
#define SPLC_RTC_PORT_H
/*
 * splc_rtc_port.h - product port (outside the simple_plc library)
 *
 * The clock of the DTU: the host's STM32 RTC (hrtc, initialised by the host's MX_RTC_Init())
 * behind plc_rtc_register(). Needs SPLC_RTC_EXTERNAL 1 in splcopts.h; compiled empty otherwise.
 * The oscillator / backup domain are the host's business: this port only reads and writes the
 * calendar registers through the HAL.
 *
 * "Synced": no is_synced hook is registered, so the library reports synced only after the App has
 * written the time since this boot (plc_rtc.c). Add the hook once the board has a backup battery.
 */
#ifdef __cplusplus
extern "C" {
#endif

/* Register the hooks. Call before plc_engine_init(), from the engine task. */
void splc_rtc_port_init(void);

#ifdef __cplusplus
}
#endif
#endif /* SPLC_RTC_PORT_H */