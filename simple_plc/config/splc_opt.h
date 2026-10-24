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

/* OPTIONAL: absolute address of the LOWEST data sector (Retain sector 1 of 3).
 * Leave it undefined (default) and the 5 data sectors are the last 5 sectors
 * of the Flash, counted back from SPLC_FLASH_SIZE_KB. Define it when a host
 * firmware (bootloader, A/B app slots, ...) already owns the end of the Flash
 * and has carved a 40 KB hole elsewhere: it must be 8 KB aligned, inside
 * bank 2, and the 5 sectors [addr, addr + 40 KB) must belong to SimplePLC
 * alone. Example: #define SPLC_FLASH_DATA_ADDR 0x081F0000UL */
/* #define SPLC_FLASH_DATA_ADDR          0x081F0000UL */

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
 * App <-> device link (Modbus RTU)
 * ==================================================================== */
/* 1 = build the optional USB-CDC port module (port/usb/): TinyUSB, the CDC
 *     descriptors, and splc_usb_cdc_init(), which registers the USB-CDC pipe
 *     with the App link (port/modbus_link). SimplePLC then OWNS the USB
 *     peripheral and its interrupt: the board file calls splc_usb_cdc_init()
 *     and forwards USB_DRD_FS_IRQHandler to splc_usb_irq(). Needed by the
 *     Zigbee-IO board.
 * 0 = no USB code at all (no TinyUSB, no descriptors). Use this when the host
 *     firmware already owns the USB peripheral: it then feeds the link with
 *     splc_link_input() and installs output() itself (port/modbus_link).
 * USB, UART and interrupts have exactly one owner, so a firmware that already
 * contains TinyUSB must leave this at 0. CMake reads this option from
 * splcopts.h. */
#ifndef SPLC_PORT_USB_CDC
#define SPLC_PORT_USB_CDC                0
#endif

/* Modbus slave address the device answers to (1..247). */
#ifndef SPLC_MODBUS_UNIT_ID
#define SPLC_MODBUS_UNIT_ID              1
#endif

/* RX byte ring of the generic App link (port/modbus_link). The port feeds it
 * with splc_link_input(); nanoMODBUS drains it. Must be a power of two and
 * hold at least one maximum Modbus RTU frame (256 bytes). It is the only RX
 * buffer of the App link, whatever the transport (USB-CDC, UART, ...). */
#ifndef SPLC_LINK_RX_BUF_SIZE
#define SPLC_LINK_RX_BUF_SIZE            512
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
 * MQTT Publish (docs/plc_mqtt_design.md section 9)
 * ==================================================================== */
/* 1 = build plc_mqtt and route Modbus 0x0C00..0x0D7F / 0x0E00..0x0E5F to it.
 * 0 = nothing changes (those addresses keep answering exception 0x02). */
#ifndef SPLC_MQTT_ENABLE
#define SPLC_MQTT_ENABLE                 0
#endif

/* ======================================================================
 * REAL (IEEE-754 float32) tags: Tag DataType Registry 0x0080..0x00BF
 * (docs/handoff.md decisions 40 and 41)
 * ==================================================================== */
/* 1 = build plc_tagtype (the table that says which tag is BOOL / INT32 / REAL;
 * staged with the Deploy, saved in the Flash Rule Table record as flag bit 11)
 * AND the float Rule Engine (step R2): the rule record's `flags` byte (wire
 * register 13, high byte) is carried over the wire / CRC / Flash / RAM, and a rule
 * with SPLC_RULE_FLAG_TRIGGER_FLOAT compares float32, and one with both float
 * flags (0x03) scales in float32 (handoff decision 55).
 * 0 = nothing changes: no table, no Flash flag, no rule flags byte, the exact
 * baseline binary (byte-identical, checked with md5 of the .bin). */
#ifndef SPLC_REAL_ENABLE
#define SPLC_REAL_ENABLE                 1
#endif

/* ======================================================================
 * MQTT Subscribe (0x0D80..0x0DFF, ICD SPLC-SPEC-MQTT-002): commands / setpoints
 * from the broker land in tags. Needs SPLC_MQTT_ENABLE = 1 (the broker config and
 * the connection status live there). The library holds the LOGIC and the data;
 * the host subscribes the topics it is asked for (splc_mqtt_sub_get()) and hands
 * every message to splc_mqtt_input() (plc_mqtt_sub.h).
 * 0 = nothing changes: no code, no Flash flag, the exact baseline binary. */
#ifndef SPLC_MQTT_SUB_ENABLE
#define SPLC_MQTT_SUB_ENABLE             0
#endif
/* Messages queued per block when the App asks for FIFO delivery (1..8). One is
 * applied per scan. 1 = latest message only (a FIFO request then behaves as such).
 * RAM: 2 blocks x depth x (128 payload + length) bytes. */
#ifndef SPLC_MQTT_SUB_FIFO_DEPTH
#define SPLC_MQTT_SUB_FIFO_DEPTH         1
#endif

/* ======================================================================
 * Modbus master blocks: MB_READ (0x1000..0x10FF), later MB_WRITE (0x1100..)
 * ==================================================================== */
/* 1 = build the Modbus-master service and enlarge the RS485 master buffers to
 * the protocol maximum (125 registers / 256-byte frame: ~450 bytes more RAM
 * per master instance). 0 = nothing changes: the master keeps its 16-register
 * / 64-byte buffers and the exact code of the Remote I/O link. */
#ifndef SPLC_MBM_ENABLE
#define SPLC_MBM_ENABLE                  1
#endif

/* Number of physical RS-485 ports the device offers to the Modbus-master blocks
 * (MB_READ / MB_WRITE): 1..4. It is what register 0x0A0A reports and what the
 * App limits its port list to; the firmware allocates one master (about 600 bytes
 * RAM) per port. Which UART serves which port is the host's business: it registers
 * one RS485 port per index with splc_mbm_set_rs485_port() (plc_mbm.h). Baud rate,
 * parity, data/stop bits, inter-frame delay and retries of every port come from the
 * App over the wire (0x1400..0x143F, SPLC-SPEC-MBM-003). */
#ifndef SPLC_MBM_PORT_COUNT
#define SPLC_MBM_PORT_COUNT              1
#endif

/* 1 = build MB_WRITE (0x1100..0x11FF, plc_mbw.h): the device writes tags to Modbus
 * slaves (FC05/06/15/16, Direct or Channel-Indexed) through the RS-485 ports of the
 * Modbus master (needs SPLC_MBM_ENABLE = 1, shares its ports and port table). 0 =
 * nothing changes: no code, no Flash flag, the exact baseline binary. */
#ifndef SPLC_MBW_ENABLE
#define SPLC_MBW_ENABLE                  0
#endif

/* 1 = build the Wi-Fi block 0x1300..0x13FF (plc_wifi.h, ICD WIFI-004): the App scans,
 * connects and configures the host's Wi-Fi through a small set of host callbacks
 * (splc_wifi_register()). SimplePLC owns no radio and no Flash for it. 0 = nothing
 * changes: no code, no RAM, the exact baseline binary. */
#ifndef SPLC_WIFI_ENABLE
#define SPLC_WIFI_ENABLE                 0
#endif

/* 1 = build the MQTT TLS certificate block 0x0F00..0x0F43 (plc_mqtt_tls.h, ICD MQTT-003):
 * the App sends the Root CA / client certificate / client key in chunks, SimplePLC checks
 * every chunk and hands it straight to the host (splc_mqtt_tls_register()); the host stores
 * it and loads it into its TLS stack. SimplePLC buffers no certificate and owns no TLS.
 * Needs SPLC_MQTT_ENABLE = 1. 0 = nothing changes: no code, no RAM. */
#ifndef SPLC_MQTT_TLS_CERT_ENABLE
#define SPLC_MQTT_TLS_CERT_ENABLE        0
#endif

/* Number of App-link transports the Modbus config server can serve at once (1..3).
 * 1 = exactly the single transport board_get_modbus_transport() returns: no extra
 * code, no extra RAM, the exact baseline binary. >1 = plc_modbus_cfg_add_transport()
 * accepts that many additional transports (e.g. Modbus TCP next to USB/RS-485), each
 * with its own nanoMODBUS server instance (RAM cost: sizeof(nmbs_t) + a transport
 * struct per extra slot). They share the draft/COMMIT/Flash state: one App at a time. */
#ifndef SPLC_MODBUS_MAX_TRANSPORTS
#define SPLC_MODBUS_MAX_TRANSPORTS       1
#endif

/* Size of the payload buffer in bytes (including the NUL). */
#ifndef SPLC_MQTT_PAYLOAD_MAX
#define SPLC_MQTT_PAYLOAD_MAX            256
#endif

/* Wait before retrying after the host publish function returned < 0 (ms). */
#ifndef SPLC_MQTT_RETRY_MS
#define SPLC_MQTT_RETRY_MS               200U
#endif

/* Lower bound of interval_ms for PERIODIC blocks (ms). */
#ifndef SPLC_MQTT_MIN_INTERVAL_MS
#define SPLC_MQTT_MIN_INTERVAL_MS        100U
#endif

/* 1 = the host client really does TLS; 0 = COMMIT rejects use_tls=1. */
#ifndef SPLC_MQTT_TLS_SUPPORTED
#define SPLC_MQTT_TLS_SUPPORTED          0
#endif

/* ======================================================================
 * Logging
 * ==================================================================== */
/* 1 = SimplePLC builds and uses its own logger (utils/logger/logger.c) and the
 *     board file calls logger_init(). The default.
 * 0 = the host firmware provides "logger.h" + the logger itself and calls
 *     logger_init() on its own (SimplePLC compiles no logger.c and does not
 *     put utils/logger on the include path). CMake reads this option from
 *     splcopts.h. See config/splc_log.h. */
#ifndef SPLC_LOGGER_BUILTIN
#define SPLC_LOGGER_BUILTIN              1
#endif

/* ======================================================================
 * Platform drivers that need a CubeMX peripheral the host may not have
 * ==================================================================== */
/* 1 (default) = SimplePLC builds its STM32H5 driver for that peripheral
 *               (platforms/stm32/stm32h5/rtc, .../adc). It needs the CubeMX
 *               rtc.h / hrtc, resp. adc.h / hadc1, in the project.
 * 0           = the host firmware provides the Layer 1 contract itself
 *               (components/rtc/sx_rtc.h: sx_rtc_set_epoch / sx_rtc_get_epoch /
 *               sx_rtc_is_synced; components/adc/sx_adc.h: sx_adc_init /
 *               sx_adc_read) and SimplePLC compiles no driver for it.
 * CMake reads both options from splcopts.h. With SPLC_RTC_BUILTIN = 0 the
 * linker options --wrap=HAL_RTC_SetTime/SetDate are not added either. */
#ifndef SPLC_RTC_BUILTIN
#define SPLC_RTC_BUILTIN                 1
#endif
#ifndef SPLC_ADC_BUILTIN
#define SPLC_ADC_BUILTIN                 1
#endif

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
#if (SPLC_PORT_USB_CDC != 0) && (SPLC_PORT_USB_CDC != 1)
#error "SPLC_PORT_USB_CDC must be 0 or 1"
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
#if (SPLC_LINK_RX_BUF_SIZE < 256) || ((SPLC_LINK_RX_BUF_SIZE & (SPLC_LINK_RX_BUF_SIZE - 1)) != 0)
#error "SPLC_LINK_RX_BUF_SIZE must be a power of two and at least 256 (one maximum Modbus RTU frame)"
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
#if (SPLC_MQTT_ENABLE != 0) && (SPLC_MQTT_ENABLE != 1)
#error "SPLC_MQTT_ENABLE must be 0 or 1"
#endif
#if (SPLC_MBM_ENABLE != 0) && (SPLC_MBM_ENABLE != 1)
#error "SPLC_MBM_ENABLE must be 0 or 1"
#endif
#if SPLC_MBM_ENABLE && ((SPLC_MBM_PORT_COUNT < 1) || (SPLC_MBM_PORT_COUNT > 4))
#error "SPLC_MBM_PORT_COUNT must be 1..4 (the wire and the Flash image carry 4 port records)"
#endif
#if (SPLC_MODBUS_MAX_TRANSPORTS < 1) || (SPLC_MODBUS_MAX_TRANSPORTS > 3)
#error "SPLC_MODBUS_MAX_TRANSPORTS must be 1..3"
#endif
#if (SPLC_WIFI_ENABLE != 0) && (SPLC_WIFI_ENABLE != 1)
#error "SPLC_WIFI_ENABLE must be 0 or 1"
#endif
#if (SPLC_MQTT_TLS_CERT_ENABLE != 0) && (SPLC_MQTT_TLS_CERT_ENABLE != 1)
#error "SPLC_MQTT_TLS_CERT_ENABLE must be 0 or 1"
#endif
#if SPLC_MQTT_TLS_CERT_ENABLE && !SPLC_MQTT_ENABLE
#error "SPLC_MQTT_TLS_CERT_ENABLE needs SPLC_MQTT_ENABLE = 1 (the certificates are for the MQTT broker connection)"
#endif
#if (SPLC_MBW_ENABLE != 0) && (SPLC_MBW_ENABLE != 1)
#error "SPLC_MBW_ENABLE must be 0 or 1"
#endif
#if SPLC_MBW_ENABLE && !SPLC_MBM_ENABLE
#error "SPLC_MBW_ENABLE = 1 needs SPLC_MBM_ENABLE = 1 (MB_WRITE shares the RS-485 ports of the Modbus master)"
#endif
#if (SPLC_REAL_ENABLE != 0) && (SPLC_REAL_ENABLE != 1)
#error "SPLC_REAL_ENABLE must be 0 or 1"
#endif
#if (SPLC_MQTT_SUB_ENABLE != 0) && (SPLC_MQTT_SUB_ENABLE != 1)
#error "SPLC_MQTT_SUB_ENABLE must be 0 or 1"
#endif
#if SPLC_MQTT_SUB_ENABLE && !SPLC_MQTT_ENABLE
#error "SPLC_MQTT_SUB_ENABLE = 1 needs SPLC_MQTT_ENABLE = 1"
#endif
#if SPLC_MQTT_SUB_ENABLE && ((SPLC_MQTT_SUB_FIFO_DEPTH < 1) || (SPLC_MQTT_SUB_FIFO_DEPTH > 8))
#error "SPLC_MQTT_SUB_FIFO_DEPTH must be 1..8"
#endif
#if (SPLC_MQTT_PAYLOAD_MAX < 128) || (SPLC_MQTT_PAYLOAD_MAX > 512)
#error "SPLC_MQTT_PAYLOAD_MAX must be 128..512 (compact JSON worst case is 127 characters + NUL)"
#endif
#if (SPLC_MQTT_RETRY_MS < 10)
#error "SPLC_MQTT_RETRY_MS must be at least 10"
#endif
#if (SPLC_MQTT_MIN_INTERVAL_MS < 10)
#error "SPLC_MQTT_MIN_INTERVAL_MS must be at least 10"
#endif
#if (SPLC_MQTT_TLS_SUPPORTED != 0) && (SPLC_MQTT_TLS_SUPPORTED != 1)
#error "SPLC_MQTT_TLS_SUPPORTED must be 0 or 1"
#endif
#if (SPLC_LOGGER_BUILTIN != 0) && (SPLC_LOGGER_BUILTIN != 1)
#error "SPLC_LOGGER_BUILTIN must be 0 or 1"
#endif
#if (SPLC_RTC_BUILTIN != 0) && (SPLC_RTC_BUILTIN != 1)
#error "SPLC_RTC_BUILTIN must be 0 or 1"
#endif
#if (SPLC_ADC_BUILTIN != 0) && (SPLC_ADC_BUILTIN != 1)
#error "SPLC_ADC_BUILTIN must be 0 or 1"
#endif
#if (SPLC_LOG_LEVEL < 0) || (SPLC_LOG_LEVEL > 4)
#error "SPLC_LOG_LEVEL must be 0..4"
#endif
#if (SPLC_LOG_BUFFER_SIZE < 128)
#error "SPLC_LOG_BUFFER_SIZE must be at least 128"
#endif

#endif /* SPLC_OPT_H */