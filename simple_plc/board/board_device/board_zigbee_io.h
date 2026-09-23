#ifndef BOARD_ZIGBEE_IO_H
#define BOARD_ZIGBEE_IO_H

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

/* USB CDC (App<->MCU Modbus channel, per docs/architecture.md section 0
 * -- USB, not RS485). Buffer sizes are a starting point, not yet tuned
 * against real Modbus RTU frame sizes/throughput. */
#define USB_RX_BUF_SIZE   256
#define USB_TX_BUF_SIZE   256

/* Modbus RTU unit ID (slave address) this board answers to. MUST be
 * 1..247: nanoMODBUS's nmbs_server_create() rejects 0 (broadcast) and
 * silently ignores any request whose unit_id byte differs from this
 * value. The App must match it (test_plc.py: --unit, default 1). */
#define MODBUS_UNIT_ID    1

#endif