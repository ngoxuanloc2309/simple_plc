#ifndef BOARD_WIFI_IO_H
#define BOARD_WIFI_IO_H

#define WIFI_IO_DI_NUM              4
#define WIFI_IO_DO_NUM              4
#define WIFI_IO_AI_NUM              0
#define WIFI_IO_AO_NUM              0
#define WIFI_IO_VFLAG_NUM           32
#define WIFI_IO_VREG_NUM            32
#define WIFI_IO_VREG_RETAIN_NUM     32
#define WIFI_IO_COUNTER_NUM         8

/*DI Configuration*/
#define DI0_PORT                    GPIOA
#define DI0_PIN                     GPIO_PIN_8   

#define DI1_PORT                    GPIOB
#define DI1_PIN                     GPIO_PIN_13  

#define DI2_PORT                    GPIOB
#define DI2_PIN                     GPIO_PIN_12  

#define DI3_PORT                    GPIOB
#define DI3_PIN                     GPIO_PIN_10

/*DO Configuration*/
#define DO0_PORT                    GPIOB
#define DO0_PIN                     GPIO_PIN_4

#define DO1_PORT                    GPIOB
#define DO1_PIN                     GPIO_PIN_5

#define DO2_PORT                    GPIOB
#define DO2_PIN                     GPIO_PIN_8

#define DO3_PORT                    GPIOB
#define DO3_PIN                     GPIO_PIN_3

/*UART Configuration*/
#define UART_LOG                    hlpuart1    /*PA9: TX, PA10: RX*/
#define UART_RS485                  huart1      /*PB14: TX, PB15: RX*/
#define UART_WIFI                   huart2      /*PA2: TX, PA3: RX*/

/*I2C Configuration*/
#define I2C_RTC                     hi2c1       /*PB6: SCL, PB7: SDA */

/*Wifi ST67 wifi configuration*/
#define ST67_CS_PORT                GPIOA
#define ST67_CS_PIN                 GPIO_PIN_4

#define ST67_RDY_PORT               GPIOB
#define ST67_RDY_PIN                GPIO_PIN_0

#define ST67_32K_PORT               GPIOB
#define ST67_32K_PIN                GPIO_PIN_1

#define ST67_WAKE_PORT              GPIOB
#define ST67_WAKE_PIN               GPIO_PIN_2

#define SPI_ST67                    hspi1       /*PA5: SCK, PA6: MISO, PA7: MOSI*/

/*RS485 Configuration*/
#define RS485_DE_PORT               GPIOA
#define RS485_DE_PIN                GPIO_PIN_15
#define RS485_DE_ACTIVE_HIGH        1           /* RDE net drives DE and /RE tied together: HIGH = transmit, LOW = listen (schematic U1) */

#include "splc_opt.h"
#define MODBUS_UNIT_ID              SPLC_MODBUS_UNIT_ID

#endif