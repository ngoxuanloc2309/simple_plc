#ifndef BOARD_DTU_H
#define BOARD_DTU_H

/*This board just has one port ETH, three RS485 ports, No DI&DO*/

#define UART_485_PORT1              huart3
#define UART_485_PORT2              huart7
#define UART_485_PORT3              huart10

#define RS485_DE_PORT1              GPIOB
#define RS485_DE_PIN1               GPIO_PIN_2

#define RS485_DE_PORT2              GPIOB
#define RS485_DE_PIN2               GPIO_PIN_12

#define RS485_DE_PORT3              GPIOB
#define RS485_DE_PIN3               GPIO_PIN_14

#define RS485_DE_ACTIVE_HIGH        1

/*LED INDICATORS*/
#define LED_POWER_PORT              GPIOC
#define LED_POWER_PIN               GPIO_PIN_15

#define LED_STATUS_PORT             GPIOC
#define LED_STATUS_PIN              GPIO_PIN_14

#define LED_LINK_PORT               GPIOC
#define LED_LINK_PIN                GPIO_PIN_13


#endif