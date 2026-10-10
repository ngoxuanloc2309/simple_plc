#ifndef SPLCOPTS_H
#define SPLCOPTS_H

/*
 * splcopts.h - SimplePLC configuration of THIS product (Zigbee-IO SKU,
 * STM32H523CCU6). Library defaults and the full option list are in
 * simple_plc/config/splc_opt.h; a blank template is
 * simple_plc/config/splcopts_template.h.
 *
 * Only options that differ from the library default (or that are worth
 * stating explicitly for this product) are listed.
 */

/* Target */
#define SPLC_PLATFORM                 SPLC_PLATFORM_STM32H5
#define SPLC_BOARD                    SPLC_BOARD_ZIGBEE_IO_4DI_4DO

/* Chip Flash size in KB (STM32H523CC = 256). Rule Table A/B + Retain are the
 * last 5 sectors of it. */
#define SPLC_FLASH_SIZE_KB            256

/* OS: 0 = bare-metal super-loop (main.c calls plc_engine_poll()),
 *     1 = FreeRTOS (one task calls plc_engine_poll()). */
#define SX_OS_USE_FREERTOS            1

/* Engine */
#define PLC_SCAN_INTERVAL_MS          10U

/* Retain: this board has no hold-up capacitor/PVD circuit yet, but the
 * emergency save is harmless when it never fires, so it stays on. */
#define PLC_PVD_EMERGENCY_SAVE_ENABLE 1

/* Logging */
#define SPLC_LOG_LEVEL                4

#endif /* SPLCOPTS_H */