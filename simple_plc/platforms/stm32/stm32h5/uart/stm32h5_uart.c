#include "stm32h5_uart.h"

#if STM32H5_PLATFORM

/*
 * HAL_UART_RxCpltCallback() only gives us the UART_HandleTypeDef* that
 * completed -- it has no field to carry our own sx_uart_t* context back.
 * We keep a small fixed-size lookup table (Instance -> sx_uart_t*),
 * registered by sx_uart_init(), so the shared ISR callback can find the
 * right queue/rx-byte-holder for whichever huart just fired. Fixed size,
 * no malloc -- consistent with the project's no-dynamic-heap rule.
 */
#define SX_UART_MAX_INSTANCES 4

typedef struct {
    UART_HandleTypeDef *hal_handle;
    sx_uart_t           *uart;
    uint8_t               rx_byte;
} sx_uart_binding_t;

static sx_uart_binding_t s_bindings[SX_UART_MAX_INSTANCES];
static int                s_binding_count = 0;

static sx_uart_binding_t *find_binding(UART_HandleTypeDef *huart)
{
    for (int i = 0; i < s_binding_count; i++) {
        if (s_bindings[i].hal_handle == huart) {
            return &s_bindings[i];
        }
    }
    return NULL;
}

static void arm_rx_byte(sx_uart_binding_t *binding)
{
    HAL_UART_Receive_IT(binding->hal_handle, &binding->rx_byte, 1);
}

void sx_uart_init(sx_uart_t *uart, sx_uart_config_t *config)
{
    uart->config = config;

    /* rxBuffer/rxBufferSize must already be set by the caller -- this is
     * caller-owned static storage, not allocated here (no malloc). */
    cqueue_init_static(&uart->rxQueue, uart->rxBuffer, (size_t)uart->rxBufferSize, sizeof(uint8_t));

    UART_HandleTypeDef *huart = (UART_HandleTypeDef *)config->pDriver;

    if (s_binding_count < SX_UART_MAX_INSTANCES) {
        sx_uart_binding_t *binding = &s_bindings[s_binding_count++];
        binding->hal_handle = huart;
        binding->uart       = uart;
        binding->rx_byte    = 0;

        /* Arm the first single-byte receive -- HAL_UART_RxCpltCallback()
         * re-arms it again after every completed byte (see below). */
        arm_rx_byte(binding);
    }
}

int sx_uart_available(sx_uart_t *uart)
{
    (void)uart;
    /* CQueue_t does not expose a byte-count getter beyond is_empty/is_full,
     * so this reports 0 (nothing) / 1 (something) only. Callers wanting an
     * exact count should read via sx_uart_read() and check the return. */
    return cqueue_is_empty(&uart->rxQueue) ? 0 : 1;
}

int sx_uart_read(sx_uart_t *uart, uint8_t *data, int len)
{
    int received = 0;

    while (received < len) {
        uint8_t byte;
        if (!cqueue_receive(&uart->rxQueue, &byte)) {
            break; /* queue empty -- return what we have so far */
        }
        data[received++] = byte;
    }

    return received;
}

int sx_uart_write(sx_uart_t *uart, const uint8_t *data, int len, uint32_t timeout_ms)
{
    UART_HandleTypeDef *huart = (UART_HandleTypeDef *)uart->config->pDriver;

    if (HAL_UART_Transmit(huart, data, (uint16_t)len, timeout_ms) != HAL_OK) {
        return -1;
    }

    return 0;
}

/*
 * Shared ISR-context callback for every UART instance registered via
 * sx_uart_init(). Pushes the just-received byte into that instance's
 * rxQueue, then immediately re-arms the next single-byte receive so RX
 * keeps flowing without the caller having to do anything.
 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    sx_uart_binding_t *binding = find_binding(huart);
    if (binding == NULL) {
        return;
    }

    /* Queue full is a silent drop here -- rxQueue sizing is a caller
     * concern (rxBufferSize), not something this driver can fix at
     * ISR time. */
    (void)cqueue_send(&binding->uart->rxQueue, &binding->rx_byte);

    arm_rx_byte(binding);
}

#endif // STM32H5_PLATFORM