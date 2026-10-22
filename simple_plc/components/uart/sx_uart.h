#ifndef __SX_UART_H__
#define __SX_UART_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "splc_cqueue.h"
#include "splc_opt.h"   /* SPLC_MBM_ENABLE: the non-blocking TX members exist only with it (all units must agree on sizeof(sx_uart_t)) */

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
#if SPLC_MBM_ENABLE
    /* Non-blocking TX state (sx_uart_write_start()). txDoneCb runs in ISR
     * context once the LAST BIT has left the shifter (TC), BEFORE txBusy drops. */
    void    (*txDoneCb)(void *ctx);
    void     *txDoneCtx;
    volatile uint8_t txBusy;
#endif
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

#if SPLC_MBM_ENABLE
/*
 * Non-blocking transmit: starts sending len bytes (interrupt driven) and returns
 * at once. 0 = started; negative = a transmit is already running, len is 0 or the
 * HAL refused. `data` is NOT copied: it must stay valid until sx_uart_tx_busy()
 * is false. Completion = last stop bit has left the UART (TC).
 *
 * NOT USED by any board today: on the Zigbee-IO board (STM32H523, USART1) this
 * path put corrupted/missing bytes on the RS485 bus although the timing was right
 * (docs/handoff.md, decision 46). Board Zigbee-IO sends with the blocking
 * sx_uart_write() instead. Kept for a later fix; measure DE/TX on the bus first.
 */
int  sx_uart_write_start(sx_uart_t *uart, const uint8_t *data, int len);
/* true while a sx_uart_write_start() transmit has not finished. */
bool sx_uart_tx_busy(const sx_uart_t *uart);
/* Hook called from the UART interrupt the moment the last bit has left (before
 * sx_uart_tx_busy() turns false). Keep it to a few register writes. NULL removes. */
void sx_uart_set_tx_done_callback(sx_uart_t *uart, void (*cb)(void *ctx), void *ctx);
/* Aborts a running sx_uart_write_start() transmit; the callback is NOT called. */
void sx_uart_write_abort(sx_uart_t *uart);
#endif /* SPLC_MBM_ENABLE */

/*
 * Re-applies uart->config (baudrate, bits, parity, stopbits) to the already
 * initialised UART: aborts the running receive, re-programs the peripheral,
 * then re-arms the single-byte receive. Used when the line settings are not
 * known at boot (MB_READ: the sensor bus speed is the App's choice). Bytes in
 * rxQueue are kept; a byte that was being shifted in is lost.
  *   bits     : 7 or 8 (the parity bit is part of the word on the STM32)
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