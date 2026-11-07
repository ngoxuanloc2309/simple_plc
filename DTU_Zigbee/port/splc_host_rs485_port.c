#include "splc_host_rs485_port.h"

#if SPLC_MBM_ENABLE

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "splc_log.h"
#include "modbus_serial.h"
#include "board_dtu_zigbee.h"   /* splc_dtu_set_rs485_port() */
#include "board.h"              /* HOST: uart_*(), RS485_n_PORT */

#if SPLC_MBM_PORT_COUNT > 3
#error "DTU Zigbee has THREE RS-485 ports: set SPLC_MBM_PORT_COUNT 1..3."
#endif

#ifndef SPLC_HOST_RS485_PORT_1
#define SPLC_HOST_RS485_PORT_1 RS485_0_PORT
#endif
#ifndef SPLC_HOST_RS485_PORT_2
#define SPLC_HOST_RS485_PORT_2 RS485_1_PORT
#endif
#ifndef SPLC_HOST_RS485_PORT_3
#define SPLC_HOST_RS485_PORT_3 RS485_2_PORT
#endif
#ifndef SPLC_HOST_RS485_BAUD
#define SPLC_HOST_RS485_BAUD 9600U
#endif

static const char *TAG = "HOST_RS485";
static plc_mbm_configure_fn s_configure;

void splc_host_rs485_port_set_configure(plc_mbm_configure_fn fn)
{
    s_configure = fn;
}

/* Non-blocking. The host's uart_read_bytes(..., 0) returns at once WITHOUT reading anything (its
 * wait loop never runs), so ask the queue how much it holds, then take exactly that much with a
 * 2-tick budget: queued bytes come out immediately, no wait on an empty queue. */
static int host_read(void *ctx, uint8_t *buf, uint16_t max_len)
{
    const int port = (int)(intptr_t)ctx;
    size_t n = 0;
    if (uart_get_buffered_data_len(port, &n) != 0 || n == 0U) {
        return 0;
    }
    if (n > max_len) {
        n = max_len;
    }
    const int r = uart_read_bytes(port, buf, (uint32_t)n, 2U);
    return (r > 0) ? r : 0;
}

/* Host uart_write_bytes(): raises DE, sends (HAL_UART_Transmit returns after the last bit), drops
 * DE. Blocks for the frame time (decision 46). The host RX interrupt stays on, so an echo of our
 * own frame lands in the queue: drop it right after the send. NOT verified on hardware. */
static int host_write_start(void *ctx, const uint8_t *buf, uint16_t len)
{
    const int port = (int)(intptr_t)ctx;
    uart_write_bytes(port, buf, len);
    uart_flush(port);
    return 0;
}

static bool host_tx_done(void *ctx)
{
    (void)ctx;
    return true;            /* the frame is out when host_write_start() returns */
}

void splc_host_rs485_port_init(void)
{
    static const int host_port[3] = { SPLC_HOST_RS485_PORT_1, SPLC_HOST_RS485_PORT_2,
                                      SPLC_HOST_RS485_PORT_3 };
    for (uint8_t i = 0; i < (uint8_t)SPLC_MBM_PORT_COUNT; i++) {
        const modbus_serial_port_t port = {
            .read        = host_read,
            .write       = NULL,
            .set_tx      = NULL,
            .ctx         = (void *)(intptr_t)host_port[i],
            .baudrate    = SPLC_HOST_RS485_BAUD,
            .write_start = host_write_start,
            .tx_done     = host_tx_done,
            .write_abort = NULL,
        };
        if (!splc_dtu_set_rs485_port(i, &port, s_configure)) {
            log_error(TAG, "App port %u not registered", (unsigned)(i + 1U));
        }
    }
}

#else
void splc_host_rs485_port_init(void) {}
#endif