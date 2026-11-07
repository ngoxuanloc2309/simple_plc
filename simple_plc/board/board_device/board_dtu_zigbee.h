#ifndef BOARD_DTU_ZIGBEE_H
#define BOARD_DTU_ZIGBEE_H

/*
 * board_dtu_zigbee.h - DTU Zigbee SKU (X.Link-GW-ZB485, hardware SX-ZB-ETH-V2.0,
 * STM32H563, CubeMX project eth_log_board / RF_Gateway.ioc)
 *
 * This board has ONE Ethernet port, THREE RS485 ports, ONE Zigbee module (DRF1609H on
 * UART9), a USB-C port, and NO DI / DO. Every pin/UART below is taken from RF_Gateway.ioc
 * (main.h labels, usart.c handles) and cross-checked with the older SynaptiX firmware of the
 * same hardware (SynaptiX/board/board.c: DE high = transmit, LEDs active high).
 *
 * What this board file does: log UART, device identity, the App link (USB-CDC), the three
 * RS485 master/slave ports (MB_READ / MB_WRITE / plc_mbs), and it calls the product ports for
 * Zigbee and the indicator LEDs. It does NOT do Ethernet: Modbus TCP / MQTT / DTU belong to the
 * host (lwIP glue), same split as the gateway SKU.
 */

/* ---- tag layout: no DI / DO / AI, the fixed internal ranges every SKU gets ------------- */
#define DTU_ZIGBEE_DI_NUM            0
#define DTU_ZIGBEE_DO_NUM            0
#define DTU_ZIGBEE_AI_NUM            0
#define DTU_ZIGBEE_VFLAG_NUM         32
#define DTU_ZIGBEE_VREG_NUM          32
#define DTU_ZIGBEE_VREG_RETAIN_NUM   32
#define DTU_ZIGBEE_COUNTER_NUM       8

/* ---- RS485 ports (index = the App's port number - 1, 0x1400 table) ---------------------
 * UART handle, driver-enable pin (RE and DE tied together: HIGH = transmit, LOW = listen). */
#define UART_485_PORT1              huart3
#define RS485_DE_PORT1              GPIOD            /* CubeMX label USART3_DE  */
#define RS485_DE_PIN1               GPIO_PIN_10

#define UART_485_PORT2              huart7
#define RS485_DE_PORT2              GPIOB            /* CubeMX label UART7_DE   */
#define RS485_DE_PIN2               GPIO_PIN_2

#define UART_485_PORT3              huart10
#define RS485_DE_PORT3              GPIOE            /* CubeMX label USART10_DE */
#define RS485_DE_PIN3               GPIO_PIN_4

#define RS485_DE_ACTIVE_HIGH        1

/* ---- other UARTs --------------------------------------------------------------------- */
#define UART_ZIGBEE                 huart9           /* PD14 TX / PD15 RX. The Zigbee port file takes this
                                                      * through SPLC_ZIGBEE_PORT_UART (put the same handle
                                                      * in the product's splcopts.h). */
#define UART_LOG                    huart1           /* PA9 TX / PA10 RX, 115200 8N1 */

/* ---- LED indicators (CubeMX labels LED2 / LED1 / LED0, active HIGH) ------------------- */
#define LED_POWER_PORT              GPIOC
#define LED_POWER_PIN               GPIO_PIN_6       /* LED2 */

#define LED_STATUS_PORT             GPIOC
#define LED_STATUS_PIN              GPIO_PIN_7       /* LED1 */

#define LED_LINK_PORT               GPIOC
#define LED_LINK_PIN                GPIO_PIN_8       /* LED0 */

#include "splc_opt.h"

/* Modbus RTU unit ID this board answers to on its App link (1..247). */
#define MODBUS_UNIT_ID              SPLC_MODBUS_UNIT_ID

#if SPLC_ZIGBEE_ENABLE
/* Product port (Zigbee_IO/port/splc_zigbee_port.c, compiled by the product): UART9 + module.
 * Prototype repeated on purpose, the board library must not need that folder on its include path. */
void splc_zigbee_port_init(void);
#endif

#if SPLC_INDICATOR_ENABLE
/* Product port (Zigbee_IO/port/splc_indicators_port.c): the three LEDs above. */
void splc_indicators_port_init(void);
#endif

#endif /* BOARD_DTU_ZIGBEE_H */