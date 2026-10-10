#ifndef SPLCOPTS_H
#define SPLCOPTS_H

/*
 * splcopts.h - YOUR product's SimplePLC configuration (template).
 *
 * Copy this file to a directory of your own project, name it splcopts.h and
 * tell CMake where it is:
 *
 *     cmake ... -DSPLC_OPTS_DIR=<that directory>
 *
 * (default: <top-level source dir>/splc_config). Uncomment and edit only
 * what you want to change; everything else keeps the default from the
 * library's config/splc_opt.h, which also documents every option and its
 * legal range. Never edit splc_opt.h itself.
 */

/* --- Target ---------------------------------------------------------- */
/* #define SPLC_PLATFORM                 SPLC_PLATFORM_STM32H5 */
/* #define SPLC_BOARD                    SPLC_BOARD_ZIGBEE_IO_4DI_4DO */   /* also CMake -DSPLC_BOARD_SKU */

/* --- Operating system ------------------------------------------------ */
/* #define SX_OS_USE_FREERTOS            1 */      /* 0 bare-metal (default), 1 FreeRTOS */

/* --- Engine ---------------------------------------------------------- */
/* #define PLC_SCAN_INTERVAL_MS          10U */
/* #define MAX_RULES                     100 */    /* 1..100 */
/* #define PLC_REBOOT_DELAY_MS           300U */

/* --- Retain ---------------------------------------------------------- */
/* #define RETAIN_SNAPSHOT_PERIOD_MS     (5U * 60U * 1000U) */
/* #define PLC_PVD_EMERGENCY_SAVE_ENABLE 1 */      /* 0 if the board has no hold-up energy */
/* #define RETAIN_EMERGENCY_MIN_INTERVAL_MS 5000U */

/* --- App <-> device link --------------------------------------------- */
/* #define SPLC_MODBUS_UNIT_ID           1 */
/* #define SPLC_USB_RX_BUF_SIZE          256 */
/* #define SPLC_USB_TX_BUF_SIZE          256 */

/* --- Logging --------------------------------------------------------- */
/* #define SPLC_LOG_LEVEL                4 */      /* 0 OFF .. 4 DEBUG */
/* #define SPLC_LOG_BUFFER_SIZE          4096 */

#endif /* SPLCOPTS_H */