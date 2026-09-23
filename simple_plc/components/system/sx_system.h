#ifndef __SX_SYSTEM_H__
#define __SX_SYSTEM_H__

#ifdef __cplusplus
extern "C" {
#endif

/*
 * sx_system.h - Layer 1 (SX Driver Core)
 *
 * Chip-agnostic contract for whole-MCU actions that don't belong to any
 * one peripheral. Today that's just "reset the MCU" -- the Layer 4 action
 * behind SPLC_SYSTEM_CMD_REBOOT (see plc_system_cmd.h, plc_modbus_cfg.c's
 * write_system_command()).
 *
 * Same split as sx_gpio.h/sx_time.h/etc: this header is the porting
 * boundary (pure contract, no HAL_*()/CMSIS calls), implemented per-chip
 * one layer down (platforms/stm32/stm32h5/system/stm32h5_system.c calls
 * NVIC_SystemReset()).
 */

/*
 * Immediately and unconditionally resets the MCU (warm reset, same as a
 * power-on reset from the application's point of view -- RAM is not
 * preserved). Does not return.
 *
 * Callers MUST ensure anything that needs to survive the reset is
 * already durable (Flash-saved) and anything that needs to reach the
 * App over the wire (e.g. a SYSTEM_COMMAND_RESULT status of
 * SPLC_CMD_STATUS_ACCEPTED/DONE) has already been handed to the
 * transport, not just staged in a RAM buffer -- this call does not wait
 * for, or know about, any pending USB/UART transmission.
 */
void sx_system_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* __SX_SYSTEM_H__ */