#ifndef SPLCOPTS_H
#define SPLCOPTS_H

/*
 * splcopts.h - SimplePLC configuration of the GatewayV2 firmware (STM32H563).
 * Library defaults and the full option list: simple_plc/config/splc_opt.h.
 * Only options that differ from the default (or worth stating) are listed.
 */

/* Target */
#define SPLC_PLATFORM                 SPLC_PLATFORM_STM32H5
#define SPLC_BOARD                    SPLC_BOARD_GATEWAY

/* STM32H563 = 2 MB Flash. The 40 KB data hole (Rule A/B + Retain) cannot be
 * the last 5 sectors here: the host already uses the whole Flash (bootloader,
 * app slots, scratch). SPLC_FLASH_DATA_ADDR places it explicitly.
 * !!! PROPOSED address, NOT yet checked against bootloader/flash_define.h and
 * !!! the linker script. Do not flash a build with it before that is done. */
#define SPLC_FLASH_SIZE_KB            2048
#define SPLC_FLASH_DATA_ADDR          0x081F0000UL

/* The host runs FreeRTOS; splc_port.c creates the single engine task. */
#define SX_OS_USE_FREERTOS            1

/* The host owns USB (usb_netif.c: TinyUSB + descriptors + ISR). */
#define SPLC_PORT_USB_CDC             0

/* Log through the host's libs/logger (LOGx macros / log_func). */
#define SPLC_LOGGER_BUILTIN           0
#define SPLC_LOG_LEVEL                3

/* The host has no PVD handler and nothing is known about a hold-up circuit
 * on the gateway: keep the emergency retain save off. */
#define PLC_PVD_EMERGENCY_SAVE_ENABLE 0

/* ---- options of ports/splc_port (defaults in splc_port.h) ---------------- */
#define SPLC_HOST_RS485_PORT          1          /* USART6 */
#define SPLC_HOST_RS485_BAUD          115200U    /* ASSUMED: I/O board baud, to be confirmed */

#endif /* SPLCOPTS_H */