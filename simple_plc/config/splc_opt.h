#ifndef SPLC_OPT_H
#define SPLC_OPT_H

/*
 * splc_opt.h - SimplePLC compile-time options and their DEFAULTS.
 *
 * Same idea as lwIP's opt.h / lwipopts.h:
 *
 *   - THIS file belongs to the library. Do not edit it to configure a
 *     product. Every option below is wrapped in #ifndef, so it only supplies
 *     a default.
 *   - Your product provides its own "splcopts.h" (copy
 *     config/splcopts_template.h) and puts the directory that holds it on
 *     the include path (CMake: -DSPLC_OPTS_DIR=<dir>, default
 *     <top-level source dir>/splc_config). Anything you #define there wins.
 *   - An option can also be passed on the compiler command line
 *     (-DPLC_SCAN_INTERVAL_MS=1U) as long as splcopts.h does not set the same
 *     option unguarded; wrap it in #ifndef there if CI should be able to
 *     override it.
 *
 * Every library file that needs an option includes THIS header and nothing
 * else for configuration. To add a new option: add it here with a default,
 * a one-line meaning and (if it has a legal range) a check at the bottom,
 * then add it to config/splcopts_template.h.
 *
 * What is NOT an option: wire-protocol constants (register addresses, struct
 * sizes, diagnostic lease limits), Flash sector layout of a chip
 * (platforms/<family>/<chip>/flash_define) and the tag layout of a board
 * (board/board_tag_define.h). They are fixed by the Wire Profile or by the
 * silicon/board and are not meant to be tuned from a config file.
 */

/* The product's own options. A missing file is a configuration error, not
 * something to silently default around (same as lwipopts.h). */
#include "splcopts.h"

/* ======================================================================
 * Selectors (values to compare against; do not change)
 * ==================================================================== */
#define SPLC_PLATFORM_STM32H5            1
#define SPLC_PLATFORM_STM32F1            2
#define SPLC_PLATFORM_STM32F4            3
#define SPLC_PLATFORM_STM32H7            4
#define SPLC_PLATFORM_ESP32              5

#define SPLC_BOARD_ZIGBEE_IO_4DI_4DO     1
#define SPLC_BOARD_REMOTE_IO_8DI_8DO     2
#define SPLC_BOARD_DATALOGGER            3
#define SPLC_BOARD_GATEWAY               4

/* ======================================================================
 * Target
 * ==================================================================== */
/* Chip family the Layer 0 (platforms/) implementation is built for.
 * Only STM32H5 has an implementation today. */
#ifndef SPLC_PLATFORM
#define SPLC_PLATFORM                    SPLC_PLATFORM_STM32H5
#endif

/* Total on-chip Flash of THIS chip, in KB (e.g. 256 for STM32H523CC, 512 for
 * STM32H523/H533 "E" parts, 2048 for STM32H563/H573). The Flash data regions
 * (Rule Table A/B + Retain, 5 sectors) are always placed at the END of the
 * Flash and counted backwards from this size, so this is the only number a
 * product has to state (platforms/<family>/<chip>/flash_define). The sector
 * size is a property of the chip family, not an option. The firmware image
 * must stay below the data region: set the linker's FLASH LENGTH to
 * SPLC_FLASH_SIZE_KB minus the reserved size (SPLC_FLASH_RESERVED_SIZE). */
#ifndef SPLC_FLASH_SIZE_KB
#define SPLC_FLASH_SIZE_KB               256
#endif

/* Product/board. Must agree with the board source CMake builds
 * (-DSPLC_BOARD_SKU=..., default zigbee_io); the board .c checks this and
 * stops with #error if they disagree. */
#ifndef SPLC_BOARD
#define SPLC_BOARD                       SPLC_BOARD_ZIGBEE_IO_4DI_4DO
#endif

/* ======================================================================
 * Operating system
 * ==================================================================== */
/* 0 = bare-metal: the host calls plc_engine_init() once and
 *     plc_engine_poll() from its main loop.
 * 1 = FreeRTOS: the host calls both from ONE task (+ a short vTaskDelay).
 *     The library creates no task. See components/os/sx_os.h for what the
 *     host project must provide (FreeRTOS headers via CMake
 *     SPLC_OS_INCLUDE_DIRS, a 1 ms HAL timebase, FreeRTOSConfig.h flags). */
#ifndef SX_OS_USE_FREERTOS
#define SX_OS_USE_FREERTOS               0
#endif

/* ======================================================================
 * Engine
 * ==================================================================== */
/* Scan period in ms. Rule dwell/interval resolution equals this value.
 * 0 = scan as fast as the loop spins (not recommended). */
#ifndef PLC_SCAN_INTERVAL_MS
#define PLC_SCAN_INTERVAL_MS             10U
#endif

/* Maximum number of rules. Sizes the Modbus staging table, the RAM rule
 * table and the rule record in Flash (all derived from this value). It can
 * only be LOWERED: the Wire Profile fixes the Active Rule Table window
 * (0x0100..0x073F) and the Staging Rule Buffer (0x9010..0x964F) at 100
 * rules, and the device reports this value to the App as max_rules. */
#ifndef MAX_RULES
#define MAX_RULES                        100
#endif

/* Delay between a REBOOT command being accepted and the reset, so the
 * Modbus reply can leave the USB FIFO first. Do not set to 0. */
#ifndef PLC_REBOOT_DELAY_MS
#define PLC_REBOOT_DELAY_MS              300U
#endif

/* ======================================================================
 * Retain (VREG_RETAIN) storage
 * ==================================================================== */
/* Period of the routine snapshot (written only when the data changed).
 * Shortening it spends Flash endurance: the endurance estimate in
 * flash_define assumes 5 minutes. */
#ifndef RETAIN_SNAPSHOT_PERIOD_MS
#define RETAIN_SNAPSHOT_PERIOD_MS        (5U * 60U * 1000U)
#endif

/* 1 = plc_engine_init() registers the emergency snapshot on the PVD
 *     (power-fail) interrupt and arms it in the NVIC.
 * 0 = PVD interrupt left disarmed (board without hold-up energy). */
#ifndef PLC_PVD_EMERGENCY_SAVE_ENABLE
#define PLC_PVD_EMERGENCY_SAVE_ENABLE    1
#endif

/* Minimum time between two PVD emergency writes (bounds Flash wear when
 * the supply chatters around the threshold). */
#ifndef RETAIN_EMERGENCY_MIN_INTERVAL_MS
#define RETAIN_EMERGENCY_MIN_INTERVAL_MS 5000U
#endif

/* ======================================================================
 * App <-> device link (Modbus RTU over USB-CDC)
 * ==================================================================== */
/* Modbus slave address the device answers to (1..247). */
#ifndef SPLC_MODBUS_UNIT_ID
#define SPLC_MODBUS_UNIT_ID              1
#endif

/* Byte queues between TinyUSB and nanoMODBUS. 256 is the tested value. */
#ifndef SPLC_USB_RX_BUF_SIZE
#define SPLC_USB_RX_BUF_SIZE             256
#endif
#ifndef SPLC_USB_TX_BUF_SIZE
#define SPLC_USB_TX_BUF_SIZE             256
#endif

/* ======================================================================
 * Remote I/O (Gateway: virtual DI/DO carried to I/O boards over RS485)
 * Used by services/plc_io/plc_io_remote.c. Unused (no node added) on boards
 * with local I/O, where it costs a few hundred bytes of RAM.
 * ==================================================================== */
/* Max I/O boards (RS485 slaves) one gateway talks to. */
#ifndef SPLC_REMOTE_MAX_NODES
#define SPLC_REMOTE_MAX_NODES            8
#endif

/* How long to wait for a slave's answer before the request counts as failed. */
#ifndef SPLC_REMOTE_TIMEOUT_MS
#define SPLC_REMOTE_TIMEOUT_MS           100U
#endif

/* Interval between DI/status reads of one node (ms). */
#ifndef SPLC_REMOTE_POLL_MS
#define SPLC_REMOTE_POLL_MS              50U
#endif

/* Consecutive failed requests after which a node is reported offline. */
#ifndef SPLC_REMOTE_OFFLINE_AFTER
#define SPLC_REMOTE_OFFLINE_AFTER        3U
#endif

/* While a node is offline, how often to try it again (ms). */
#ifndef SPLC_REMOTE_RETRY_MS
#define SPLC_REMOTE_RETRY_MS             1000U
#endif

/* ======================================================================
 * Logging
 * ==================================================================== */
/* Initial log level: 0 OFF, 1 ERROR, 2 WARNING, 3 INFO, 4 DEBUG. */
#ifndef SPLC_LOG_LEVEL
#define SPLC_LOG_LEVEL                   4
#endif

/* Size of the shared line buffer in logger.c. Longer lines are truncated. */
#ifndef SPLC_LOG_BUFFER_SIZE
#define SPLC_LOG_BUFFER_SIZE             4096
#endif

/* ======================================================================
 * Sanity checks - a bad option should stop the build, not the device
 * ==================================================================== */
#if (SX_OS_USE_FREERTOS != 0) && (SX_OS_USE_FREERTOS != 1)
#error "SX_OS_USE_FREERTOS must be 0 or 1"
#endif
#if (PLC_PVD_EMERGENCY_SAVE_ENABLE != 0) && (PLC_PVD_EMERGENCY_SAVE_ENABLE != 1)
#error "PLC_PVD_EMERGENCY_SAVE_ENABLE must be 0 or 1"
#endif
#if (SPLC_PLATFORM < SPLC_PLATFORM_STM32H5) || (SPLC_PLATFORM > SPLC_PLATFORM_ESP32)
#error "SPLC_PLATFORM must be one of the SPLC_PLATFORM_* selectors"
#endif
#if (SPLC_BOARD < SPLC_BOARD_ZIGBEE_IO_4DI_4DO) || (SPLC_BOARD > SPLC_BOARD_GATEWAY)
#error "SPLC_BOARD must be one of the SPLC_BOARD_* selectors"
#endif
#if (SPLC_FLASH_SIZE_KB < 16)
#error "SPLC_FLASH_SIZE_KB must be the chip's Flash size in KB (at least 16)"
#endif
#if (MAX_RULES < 1) || (MAX_RULES > 100)
#error "MAX_RULES must be 1..100 (Wire Profile V2.0 register windows hold 100 rules)"
#endif
#if (PLC_REBOOT_DELAY_MS < 50)
#error "PLC_REBOOT_DELAY_MS below 50 ms can reset before the Modbus reply is sent"
#endif
#if (SPLC_MODBUS_UNIT_ID < 1) || (SPLC_MODBUS_UNIT_ID > 247)
#error "SPLC_MODBUS_UNIT_ID must be 1..247"
#endif
#if (SPLC_USB_RX_BUF_SIZE < 64) || (SPLC_USB_TX_BUF_SIZE < 64)
#error "SPLC_USB_RX_BUF_SIZE / SPLC_USB_TX_BUF_SIZE must be at least 64 (one USB packet)"
#endif
#if (SPLC_REMOTE_MAX_NODES < 1) || (SPLC_REMOTE_MAX_NODES > 32)
#error "SPLC_REMOTE_MAX_NODES must be 1..32"
#endif
#if (SPLC_REMOTE_TIMEOUT_MS < 20) || (SPLC_REMOTE_TIMEOUT_MS > 2000)
#error "SPLC_REMOTE_TIMEOUT_MS must be 20..2000"
#endif
#if (SPLC_REMOTE_POLL_MS < 10) || (SPLC_REMOTE_POLL_MS > 1000)
#error "SPLC_REMOTE_POLL_MS must be 10..1000"
#endif
#if (SPLC_REMOTE_OFFLINE_AFTER < 1) || (SPLC_REMOTE_OFFLINE_AFTER > 100)
#error "SPLC_REMOTE_OFFLINE_AFTER must be 1..100"
#endif
#if (SPLC_REMOTE_RETRY_MS < 100)
#error "SPLC_REMOTE_RETRY_MS below 100 ms would flood a dead RS485 link"
#endif
#if (SPLC_LOG_LEVEL < 0) || (SPLC_LOG_LEVEL > 4)
#error "SPLC_LOG_LEVEL must be 0..4"
#endif
#if (SPLC_LOG_BUFFER_SIZE < 128)
#error "SPLC_LOG_BUFFER_SIZE must be at least 128"
#endif

#endif /* SPLC_OPT_H */