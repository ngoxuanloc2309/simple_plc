#include "splc_host_rs485_port.h"

#if SPLC_MBM_ENABLE

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "main.h"                /* HAL GPIO, DE pin labels */
#include "usart.h"               /* huart3 / huart7 / huart10 */
#include "FreeRTOS.h"
#include "dev/uart/sx_uart.h"    /* the HOST's sx_uart (the library's components/uart/sx_uart.h has
                                  * the same file name but another API: never include that one) */
#include "splc_log.h"
#include "modbus_serial.h"
#include "board_dtu_zigbee.h"    /* splc_dtu_set_rs485_port(), UART_485_PORTn, RS485_DE_*n */

#if SPLC_MBM_PORT_COUNT > 3
#error "DTU Zigbee has THREE RS-485 ports: set SPLC_MBM_PORT_COUNT 1..3."
#endif

#ifndef SPLC_HOST_RS485_TX_TIMEOUT_MS
#define SPLC_HOST_RS485_TX_TIMEOUT_MS 1000U
#endif

typedef struct {
    sx_uart_port_t       uart;
    UART_HandleTypeDef  *huart;
    GPIO_TypeDef        *de_port;
    uint16_t             de_pin;
} rs485_hw_t;

/* Index = App port number - 1. */
static const rs485_hw_t s_hw[3] = {
    { SX_UART_PORT_UART7,   &UART_485_PORT1, RS485_DE_PORT1, RS485_DE_PIN1 },
    { SX_UART_PORT_USART3,  &UART_485_PORT2, RS485_DE_PORT2, RS485_DE_PIN2 },
    { SX_UART_PORT_USART10, &UART_485_PORT3, RS485_DE_PORT3, RS485_DE_PIN3 },
};

static const char *TAG = "HOST_RS485";
static plc_mbm_configure_fn s_configure;

void splc_host_rs485_port_set_configure(plc_mbm_configure_fn fn)
{
    s_configure = fn;
}

static void de_set(const rs485_hw_t *hw, bool transmit)
{
    const GPIO_PinState level = (transmit == (RS485_DE_ACTIVE_HIGH != 0)) ? GPIO_PIN_SET
                                                                          : GPIO_PIN_RESET;
    HAL_GPIO_WritePin(hw->de_port, hw->de_pin, level);
}

/* Non-blocking: ask how much the sx_uart RX stream holds, take exactly that much. */
static int host_read(void *ctx, uint8_t *buf, uint16_t max_len)
{
    const rs485_hw_t *hw = (const rs485_hw_t *)ctx;
    if (!sx_uart_is_installed(hw->uart)) {
        return 0;
    }
    int32_t n = sx_uart_get_rx_bytes_available(hw->uart);
    if (n <= 0) {
        return 0;               /* nothing, or -1 = polling mode (no RX buffer, see the header) */
    }
    if (n > (int32_t)max_len) {
        n = (int32_t)max_len;
    }
    const int32_t r = sx_uart_read(hw->uart, buf, (size_t)n, 0U);
    return (r > 0) ? (int)r : 0;
}

/* Blocking for the frame time (decision 46): DE high, send, DE low. RE is tied to DE, so the
 * receiver is off while we transmit and no echo reaches the RX stream. NOT verified on hardware. */
static int host_write_start(void *ctx, const uint8_t *buf, uint16_t len)
{
    const rs485_hw_t *hw = (const rs485_hw_t *)ctx;
    if (!sx_uart_is_installed(hw->uart)) {
        return -1;
    }
    de_set(hw, true);
    const int32_t w = sx_uart_write(hw->uart, buf, len,
                                    pdMS_TO_TICKS(SPLC_HOST_RS485_TX_TIMEOUT_MS));
    de_set(hw, false);
    return (w == (int32_t)len) ? 0 : -1;
}

static bool host_tx_done(void *ctx)
{
    (void)ctx;
    return true;                /* the frame is out when host_write_start() returns */
}

void splc_host_rs485_port_init(void)
{
    for (uint8_t i = 0; i < (uint8_t)SPLC_MBM_PORT_COUNT; i++) {
        const modbus_serial_port_t port = {
            .read        = host_read,
            .write       = NULL,
            .set_tx      = NULL,
            .ctx         = (void *)&s_hw[i],
            .baudrate    = s_hw[i].huart->Init.BaudRate,
            .write_start = host_write_start,
            .tx_done     = host_tx_done,
            .write_abort = NULL,
        };
        de_set(&s_hw[i], false);    /* listen */
        if (!splc_dtu_set_rs485_port(i, &port, s_configure)) {
            log_error(TAG, "App port %u not registered", (unsigned)(i + 1U));
        }
    }
}

#else
void splc_host_rs485_port_init(void) {}
#endif
