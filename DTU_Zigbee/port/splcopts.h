#ifndef SPLCOPTS_H
#define SPLCOPTS_H
/*
 * splcopts.h - SimplePLC options of the DTU Zigbee product (STM32H563, board SX-ZB-ETH-V2.0,
 * host firmware: eth_log_board / DTU_Firmware.ioc).
 * Pass the directory of this file to CMake: -DSPLC_OPTS_DIR=<...>/DTU_Zigbee/port
 * CMake reads SPLC_MODBUS_LIB and the USB/RTC options at CONFIGURE time: after changing them
 * delete the build directory.
 */

/* Target */
#define SPLC_PLATFORM                 SPLC_PLATFORM_STM32H5
#define SPLC_BOARD                    SPLC_BOARD_DTU_ZIGBEE
#define SPLC_FLASH_SIZE_KB            2048

/* OS: the host runs FreeRTOS; splc_port.c creates ONE task that calls plc_engine_poll(). */
#define SX_OS_USE_FREERTOS            1

/* USB: SimplePLC owns the USB peripheral through its own port module port/usb/ (TinyUSB, CDC
 * instance 0, descriptors, interrupt). The host has NO TinyUSB. Host-side requirement: the
 * CubeMX USB_DRD_FS interrupt handler must NOT be generated (.ioc: NVIC, USB global interrupt,
 * untick "Generate IRQ handler"), because board_dtu_zigbee.c defines USB_DRD_FS_IRQHandler(). */
#define SPLC_PORT_USB_CDC             1

#define PLC_SCAN_INTERVAL_MS          10U
#define PLC_PVD_EMERGENCY_SAVE_ENABLE 1
#define SPLC_LOG_LEVEL                4

/* Modbus: MB library, three RS-485 ports (UART7 / USART3 / USART10, the host's settings order) */
#define SPLC_MODBUS_LIB               SPLC_MODBUS_LIB_MB
#define SPLC_MBM_ENABLE               1
#define SPLC_MBM_PORT_COUNT           3
#define SPLC_MBW_ENABLE               1
#define SPLC_MODBUS_SLAVE_ENABLE      1
#define SPLC_REAL_ENABLE              1

/* App Studio link: transport 0 = USB-CDC (RTU, port/usb), transport 1 = Modbus TCP
 * (port/splc_tcp_port.c) */
#define SPLC_MODBUS_MAX_TRANSPORTS    2

/* MQTT: the publish backend is plugged in by the host (port/splc_mqtt_port.h) */
#define SPLC_MQTT_ENABLE              1

/* Clock: the host's STM32 RTC (hrtc) behind plc_rtc_register() (port/splc_rtc_port.c) */
#define SPLC_RTC_EXTERNAL             1

/* The HOST owns the logger and every UART (SimplePLC = logic only, no UART driver): the library
 * builds no logger.c and no sx_uart driver; its log goes to the host's log_func(). RS485 and
 * Zigbee byte ports: port/splc_host_rs485_port.c, splc_host_zigbee_port.c (built on the host's
 * sx_uart_*, board/dev/uart/sx_uart.h). */
#define SPLC_LOGGER_BUILTIN           0
#define SPLC_UART_BUILTIN             0

/* Zigbee DRF1609H on UART9. The host's rf_app owns UART9 (see splc_host_zigbee_port.c). */
#define SPLC_ZIGBEE_ENABLE            1

/* The host's indicator_app already drives LED2/LED1/LED0 (PC6/PC7/PC8) through dtu_board.c:
 * two owners of the same pins would fight. 0 = the host keeps the LEDs. */
#define SPLC_INDICATOR_ENABLE         0

/* ---- product-port options (read by the port sources only) ---- */
/* TCP port of the App link (assumption: standard Modbus TCP port; change if App Studio differs) */
#define SPLC_TCP_PORT_NUMBER          502
/* Milliseconds a blocking RS485 frame write may take (sx_uart_write timeout) */
#define SPLC_HOST_RS485_TX_TIMEOUT_MS 1000U
#endif /* SPLCOPTS_H */