#ifndef SPLCOPTS_H
#define SPLCOPTS_H

/* Target */
#define SPLC_PLATFORM                 SPLC_PLATFORM_STM32H5
#define SPLC_BOARD                    SPLC_BOARD_ZIGBEE_IO_4DI_4DO

/* Chip Flash size in KB (STM32H523CC = 256). Rule Table A/B + Retain are the
 * last 5 sectors of it. */
#define SPLC_FLASH_SIZE_KB            256

/* OS: 0 = bare-metal super-loop (main.c calls plc_engine_poll()),
 *     1 = FreeRTOS (one task calls plc_engine_poll()). */
#define SX_OS_USE_FREERTOS            1

/* App <-> device link: RS485 Modbus RTU slave on USART1 (DE = PB13), written in
 * board_zigbee_io.c. 0 = no USB code is built (no TinyUSB). Use the sibling
 * directory splc_config/ for the USB-CDC variant. */
#define SPLC_PORT_USB_CDC             1

/* Engine */
#define PLC_SCAN_INTERVAL_MS          10U

/* Retain: this board has no hold-up capacitor/PVD circuit yet, but the
 * emergency save is harmless when it never fires, so it stays on. */
#define PLC_PVD_EMERGENCY_SAVE_ENABLE 1

/* Logging */
#define SPLC_LOG_LEVEL                4

/*ON MQTT*/
#define SPLC_MQTT_ENABLE              1

/*ON MB*/
#define SPLC_MBM_ENABLE               1

/*ON REAL PROCESS*/
#define SPLC_REAL_ENABLE              1

/*ON MB WRITE*/
#define SPLC_MBW_ENABLE               1

#endif /* SPLCOPTS_H */