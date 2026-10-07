#ifndef STM32H5_RTC_H
#define STM32H5_RTC_H

#include "sx_platform_config.h"

#if STM32H5_PLATFORM

#include "stm32h5xx_hal.h"
#include "sx_rtc.h"

/*
 * "Synced" marker kept in a TAMP backup register. Backup registers live in
 * the backup domain: they survive a warm reset together with the RTC
 * counters, and are cleared together with them when VDD is lost (no VBAT on
 * this board) -- exactly the lifetime "synced" needs.
 *
 * DR8, not DR0..DR7: TAMP_BKP0R..BKP7R become unreadable if the boot
 * hardware key is ever locked (see stm32h5xx_hal_rtc_ex.c), DR8 and up do
 * not.
 */
#define STM32H5_RTC_SYNC_BKP_REG   RTC_BKP_DR8
#define STM32H5_RTC_SYNC_MAGIC     0x53504C43UL   /* "SPLC" */

#endif // STM32H5_PLATFORM

#endif // STM32H5_RTC_H