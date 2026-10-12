#ifndef BOARD_ZIGBEE_IO_H
#define BOARD_ZIGBEE_IO_H

#define ZIGBEE_IO_DI_NUM    4
#define ZIGBEE_IO_DO_NUM    4
#define ZIGBEE_IO_AI_NUM    0
#define ZIGBEE_IO_VFLAG_NUM        32
#define ZIGBEE_IO_VREG_NUM         32
#define ZIGBEE_IO_VREG_RETAIN_NUM  32
#define ZIGBEE_IO_COUNTER_NUM      8

/*This is the board for test PLC_SIMPLE, this board has 4DI, 4DO, 0AI*/
#define DI0_PORT    GPIOB
#define DI0_PIN     GPIO_PIN_6

#define DI1_PORT    GPIOB
#define DI1_PIN     GPIO_PIN_5

#define DI2_PORT    GPIOB
#define DI2_PIN     GPIO_PIN_4

#define DI3_PORT    GPIOB
#define DI3_PIN     GPIO_PIN_3

#define DO0_PORT    GPIOA
#define DO0_PIN     GPIO_PIN_6

#define DO1_PORT    GPIOA
#define DO1_PIN     GPIO_PIN_7

#define DO2_PORT    GPIOB
#define DO2_PIN     GPIO_PIN_0

#define DO3_PORT    GPIOB
#define DO3_PIN     GPIO_PIN_1

/* UART_LOG: pDriver in board_hw_init() points at &hlpuart1 (extern in
 * Core/Inc/usart.h, CubeMX-generated) -- LPUART1_IRQHandler already
 * exists in Core/Src/stm32h5xx_it.c, so RX interrupt-driven logging
 * works without further .ioc changes. */
#define UART_LOG    hlpuart1
#define UART_RS485  huart1
#define UART_ZIGBEE huart2
#define ZIGBEE_TIM1 htim1

/* RS485 App link (used when SPLC_PORT_USB_CDC = 0 in splcopts.h): USART1 with a
 * GPIO-driven transceiver direction pin (CubeMX label USART1_DE, PB13 -- a
 * plain output, not the USART's hardware DE function). Baud rate and parity
 * are whatever CubeMX configured for USART1 (115200 8N1). */
#define RS485_DE_PORT         USART1_DE_GPIO_Port
#define RS485_DE_PIN          USART1_DE_Pin
#define RS485_DE_ACTIVE_HIGH  1      /* RDE net drives DE and /RE tied together: HIGH = transmit, LOW = listen (schematic U1) */
/* Temporary bring-up diagnostics (RX/TX log + a summary every 2 s). Set to 0 once the link works. */
#define RS485_DIAG            1
/* Bring-up only: 1 = every 2 s send 8 x 0x55 on the bus (open the PC port in a terminal and
 * check it receives them -> proves the TX path + DE polarity). Set to 0 once the link works. */
#define RS485_TX_TEST         1
#define RS485_TX_TIMEOUT_MS   100U   /* longest frame: 256 bytes = ~22 ms at 115200 */

/* USB CDC (App<->MCU Modbus channel, per docs/architecture.md section 0
 * -- USB, not RS485) is the optional port module port/usb/; its byte
 * buffering is the link ring (SPLC_LINK_RX_BUF_SIZE, splcopts.h), not a board
 * constant. */
#include "splc_opt.h"

/* Modbus RTU unit ID (slave address) this board answers to. MUST be
 * 1..247: nanoMODBUS's nmbs_server_create() rejects 0 (broadcast) and
 * silently ignores any request whose unit_id byte differs from this
 * value. The App must match it (test_plc.py: --unit, default 1). */
#define MODBUS_UNIT_ID    SPLC_MODBUS_UNIT_ID   /* product option, splcopts.h */

#endif