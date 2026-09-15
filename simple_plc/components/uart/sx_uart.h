#ifndef __SX_UART_H__
#define __SX_UART_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "cqueue.h"

typedef struct sx_uart_config {
    void *pDriver;
    uint32_t baudrate;
    uint8_t bits;
    uint8_t parity;
    uint8_t stopbits;
} sx_uart_config_t;

typedef struct sx_uart{
    sx_uart_config_t *config;
    uint8_t *rxBuffer;
    int rxBufferSize;
    CQueue_t rxQueue;

    uint8_t *txBuffer;
    int txBufferSize;
} sx_uart_t;

void sx_uart_init(); //param: instance, baudrate, parity, stopbits, databits

#ifdef __cplusplus
}
#endif

#endif