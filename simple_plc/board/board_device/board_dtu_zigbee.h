#ifndef BOARD_DTU_ZIGBEE_H
#define BOARD_DTU_ZIGBEE_H

/*
 * board_dtu_zigbee.h - DTU Zigbee SKU (X.Link-GW-ZB485, hardware SX-ZB-ETH-V2.0, STM32H563,
 * host firmware eth_log_board / DTU_Firmware.ioc)
 *
 * This board has ONE Ethernet port, THREE RS485 ports, ONE Zigbee module (DRF1609H on UART9),
 * a USB-C port, and NO DI / DO. Every pin/UART below is taken from DTU_Firmware.ioc (main.h
 * labels, usart.c handles) and the host's sx_uart port order.
 *
 * What this board file does: device identity, the App link over USB-CDC (the library's own port
 * module, SPLC_PORT_USB_CDC = 1), the three RS485 master/slave ports (MB_READ / MB_WRITE /
 * plc_mbs). It does NOT do Ethernet: the host owns the lwIP glue; Modbus TCP is attached by the
 * product port (splc_tcp_port.c) as a second App transport. The host owns the logger and every
 * UART. Requires SPLC_PORT_USB_CDC 1 for the USB link; the host must not generate the CubeMX
 * USB_DRD_FS interrupt handler (this library supplies USB_DRD_FS_IRQHandler()).
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
 * Same order as the host's settings and sx_uart ports: UART7, USART3, USART10.
 * The DE pins are GPIO outputs (RE and DE tied together: HIGH = transmit, LOW = listen); the
 * host leaves them low and does not drive them: splc_host_rs485_port.c does. */
#define UART_485_PORT1              huart7
#define RS485_DE_PORT1              UART7_DE_GPIO_Port    /* GPIOB */
#define RS485_DE_PIN1               UART7_DE_Pin          /* PB2   */

#define UART_485_PORT2              huart3
#define RS485_DE_PORT2              USART3_DE_GPIO_Port   /* GPIOD */
#define RS485_DE_PIN2               USART3_DE_Pin         /* PD10  */

#define UART_485_PORT3              huart10
#define RS485_DE_PORT3              USART10_DE_GPIO_Port  /* GPIOE */
#define RS485_DE_PIN3               USART10_DE_Pin        /* PE4   */

#define RS485_DE_ACTIVE_HIGH        1

/* ---- other UARTs (documentation; the host installs them with sx_uart) ------------------ */
#define UART_ZIGBEE                 huart9           /* PD14 TX / PD15 RX, owned by the host's rf_app */
#define UART_LOG                    hlpuart1         /* LPUART1, PA9 TX / PA10 RX (AF3), 115200 8N1 */

/* ---- LED indicators (CubeMX labels LED2 / LED1 / LED0, active HIGH) -------------------
 * The host's indicator_app drives them (dtu_board.c). Used here only if SPLC_INDICATOR_ENABLE
 * is turned on again, and then the host must stop driving them. */
#define LED_POWER_PORT              GPIOC
#define LED_POWER_PIN               GPIO_PIN_6       /* LED2 */

#define LED_STATUS_PORT             GPIOC
#define LED_STATUS_PIN              GPIO_PIN_7       /* LED1 */

#define LED_LINK_PORT               GPIOC
#define LED_LINK_PIN                GPIO_PIN_8       /* LED0 */

#include "splc_opt.h"
#if SPLC_MBM_ENABLE
#include "modbus_serial.h"
#include "plc_mbm.h"         /* plc_mbm_configure_fn */
#endif

/* Modbus RTU unit ID this board answers to on its App link (1..247). */
#define MODBUS_UNIT_ID              SPLC_MODBUS_UNIT_ID

#if SPLC_MBM_ENABLE
/* Called by the product port (splc_host_rs485_port.c) BEFORE plc_engine_init(). */
bool splc_dtu_set_rs485_port(uint8_t port_index, const modbus_serial_port_t *port,
                             plc_mbm_configure_fn configure);
#endif

#if SPLC_INDICATOR_ENABLE
/* Product port (Zigbee_IO/port/splc_indicators_port.c): the three LEDs above. */
void splc_indicators_port_init(void);
#endif

#endif /* BOARD_DTU_ZIGBEE_H */