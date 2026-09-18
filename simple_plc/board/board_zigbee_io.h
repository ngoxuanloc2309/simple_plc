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

#define UART_LOG    lpuart1

#endif