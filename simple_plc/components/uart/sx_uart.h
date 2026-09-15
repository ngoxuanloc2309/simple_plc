#ifndef __SX_UART_H__
#define __SX_UART_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "cqueue.h"

typedef struct sx_uart_config {
    void     *pDriver;   /* points to the real HAL handle, e.g. &huart1 */
    uint32_t  baudrate;
    uint8_t   bits;
    uint8_t   parity;
    uint8_t   stopbits;
} sx_uart_config_t;

typedef struct sx_uart {
    sx_uart_config_t *config;

    /* RX: filled 1 byte at a time by HAL_UART_RxCpltCallback (Layer 0 ISR
     * context), drained by sx_uart_read() (Layer 1, normal context).
     * rxQueue is initialized with cqueue_init_static() over rxBuffer --
     * no malloc, per the project's no-dynamic-heap rule (see
     * docs/architecture.md, Design principles). */
    uint8_t  *rxBuffer;
    int       rxBufferSize;
    CQueue_t  rxQueue;

    /* TX buffer is caller-owned scratch space; sx_uart_write() does not
     * queue -- it hands data directly to HAL_UART_Transmit_IT(). */
    uint8_t  *txBuffer;
    int       txBufferSize;
} sx_uart_t;

/*
 * Wires uart->config = config, initializes rxQueue over rxBuffer/
 * rxBufferSize (already set by the caller on *uart before calling this),
 * and arms the first single-byte HAL_UART_Receive_IT() so RX starts
 * flowing into rxQueue immediately.
 *
 * Caller must set uart->rxBuffer/rxBufferSize (and optionally txBuffer/
 * txBufferSize) BEFORE calling sx_uart_init() -- this function does not
 * allocate them.
 */
void sx_uart_init(sx_uart_t *uart, sx_uart_config_t *config);

/*
 * Drains up to len bytes already received into rxQueue (non-blocking --
 * returns immediately with whatever is available, does not wait for
 * more). Returns the number of bytes actually copied into data.
 */
int sx_uart_read(sx_uart_t *uart, uint8_t *data, int len);

/*
 * Returns the number of bytes currently buffered in rxQueue, ready to be
 * read without blocking.
 */
int sx_uart_available(sx_uart_t *uart);

/*
 * Blocking transmit of len bytes via HAL_UART_Transmit (polling), up to
 * timeout_ms. Returns 0 on success, negative on error/timeout.
 */
int sx_uart_write(sx_uart_t *uart, const uint8_t *data, int len, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif