#ifndef SPLCOPTS_H
#define SPLCOPTS_H
/*
 * splcopts.h - SimplePLC options of the DTU Zigbee product (STM32H563, board SX-ZB-ETH-V2.0).
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

/* USB: the HOST owns the USB peripheral (TinyUSB, handoff decision 15). The board file
 * #errors if this is 1. The App link over USB-CDC is port/splc_usb_port.c (CDC instance 1). */
#define SPLC_PORT_USB_CDC             0

#define PLC_SCAN_INTERVAL_MS          10U
#define PLC_PVD_EMERGENCY_SAVE_ENABLE 1
#define SPLC_LOG_LEVEL                4

/* Modbus: MB library, three RS-485 ports (USART3 / UART7 / USART10) */
#define SPLC_MODBUS_LIB               SPLC_MODBUS_LIB_MB
#define SPLC_MBM_ENABLE               1
#define SPLC_MBM_PORT_COUNT           3
#define SPLC_MBW_ENABLE               1
#define SPLC_MODBUS_SLAVE_ENABLE      1
#define SPLC_REAL_ENABLE              1

/* App Studio link: transport 0 = USB-CDC (RTU), transport 1 = Modbus TCP (port/splc_tcp_port.c) */
#define SPLC_MODBUS_MAX_TRANSPORTS    2

/* MQTT: the publish backend is plugged in by the host (port/splc_mqtt_port.h) */
#define SPLC_MQTT_ENABLE              1

/* Clock: the host's STM32 RTC (hrtc) behind plc_rtc_register() (port/splc_rtc_port.c) */
#define SPLC_RTC_EXTERNAL             1

/* The HOST owns the logger and every UART (SimplePLC = logic only, no hardware): the library builds
 * no logger.c and puts no logger.h of its own on the include path; its log goes to the host's
 * log_func(). RS485 and Zigbee byte ports: port/splc_host_rs485_port.c, splc_host_zigbee_port.c
 * (built on the host's uart_*()). */
#define SPLC_LOGGER_BUILTIN           0
/* No UART driver in the library (it would define HAL_UART_RxCpltCallback() a second time). */
#define SPLC_UART_BUILTIN             0

/* Zigbee DRF1609H (UART9 = the host's RF_PORT); connection indicator LEDs PC6/PC7/PC8
 * (LED2/LED1/LED0 in main.h, GPIO writes only: the host's MX_GPIO_Init sets the pins up) */
#define SPLC_ZIGBEE_ENABLE            1
#define SPLC_INDICATOR_ENABLE         1

/* ---- product-port options (read by the port sources only) ---- */
/* TCP port of the App link (assumption: standard Modbus TCP port; change if App Studio differs) */
#define SPLC_TCP_PORT_NUMBER          502
/* Line speed told to the library for the host-owned UARTs (must equal the real speed) */
#define SPLC_HOST_RS485_BAUD          9600U
#define SPLC_HOST_ZIGBEE_BAUD         115200U
/* TinyUSB CDC instance used by SimplePLC (0 = host shell, 1 = added in usb_descriptors.c) */
#define SPLC_USB_CDC_ITF              1
#endif /* SPLCOPTS_H */