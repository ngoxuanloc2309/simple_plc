#ifndef __SX_UART_H__
#define __SX_UART_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "splc_cqueue.h"

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
     * rxQueue is initialized with splc_cqueue_init_static() over rxBuffer --
     * no malloc, per the project's no-dynamic-heap rule (see
     * docs/architecture.md, Design principles). */
    uint8_t  *rxBuffer;
    int       rxBufferSize;
    splc_cqueue_t  rxQueue;

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

/*
 * Half-duplex helper (RS485): enable/disable the UART receiver without
 * touching the rest of the driver. Disabling it keeps the transceiver's own
 * echo of a transmitted frame out of rxQueue; enabling it again discards
 * anything still sitting in the hardware receive register and clears stale
 * error flags, so the next received byte is a real one. The single-byte
 * interrupt receive armed by sx_uart_init() stays armed throughout.
 */
void sx_uart_set_rx_enabled(sx_uart_t *uart, bool enabled);

/*
 * Re-applies uart->config (baudrate, bits, parity, stopbits) to the already
 * initialised UART: aborts the running receive, re-programs the peripheral,
 * then re-arms the single-byte receive. Used when the line settings are not
 * known at boot (MB_READ: the sensor bus speed is the App's choice). Bytes in
 * rxQueue are kept; a byte that was being shifted in is lost.
 *   bits     : 8 only
 *   parity   : 0 none, 1 odd, 2 even
 *   stopbits : 1 or 2
 * Returns 0 on success, negative (nothing changed) for an unsupported value
 * or when uart was never passed to sx_uart_init(). Call from normal context,
 * never from the UART interrupt.
 */
int sx_uart_reconfigure(sx_uart_t *uart);

#ifdef __cplusplus
}
#endif

#endif