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
#if SPLC_MBM_ENABLE
    uart->txBusy = 0U;
#endif

    /* rxBuffer/rxBufferSize must already be set by the caller -- this is
     * caller-owned static storage, not allocated here (no malloc). */
    splc_cqueue_init_static(&uart->rxQueue, uart->rxBuffer, (size_t)uart->rxBufferSize, sizeof(uint8_t));

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

/*
 * rxQueue has one producer (HAL_UART_RxCpltCallback, ISR context) and one
 * consumer (sx_uart_read/available, normal context), but splc_cqueue_t keeps a
 * shared `count` that BOTH sides modify with a plain read-modify-write. If the
 * ISR fires between the consumer's load and store of `count`, one update is
 * lost and the queue permanently disagrees with itself (a byte is replayed or
 * dropped -> random Modbus CRC errors). The consumer therefore takes each byte
 * (and each empty-check) with interrupts masked; PRIMASK is saved/restored so
 * this is also safe if the caller already runs with interrupts disabled. The
 * critical section is a handful of instructions per byte.
 */
int sx_uart_available(sx_uart_t *uart)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    bool empty = splc_cqueue_is_empty(&uart->rxQueue);
    __set_PRIMASK(primask);

    /* splc_cqueue_t does not expose a byte-count getter beyond is_empty/is_full,
     * so this reports 0 (nothing) / 1 (something) only. Callers wanting an
     * exact count should read via sx_uart_read() and check the return. */
    return empty ? 0 : 1;
}

int sx_uart_read(sx_uart_t *uart, uint8_t *data, int len)
{
    int received = 0;

    while (received < len) {
        uint8_t byte;

        uint32_t primask = __get_PRIMASK();
        __disable_irq();
        bool got = splc_cqueue_receive(&uart->rxQueue, &byte);
        __set_PRIMASK(primask);

        if (!got) {
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

#if SPLC_MBM_ENABLE
int sx_uart_write_start(sx_uart_t *uart, const uint8_t *data, int len)
{
    UART_HandleTypeDef *huart = (UART_HandleTypeDef *)uart->config->pDriver;
    if (len <= 0 || len > 0xFFFF || uart->txBusy) { return -1; }
    uart->txBusy = 1U;
    if (HAL_UART_Transmit_IT(huart, data, (uint16_t)len) != HAL_OK) { uart->txBusy = 0U; return -1; }
    return 0;
}

bool sx_uart_tx_busy(const sx_uart_t *uart) { return uart->txBusy != 0U; }

void sx_uart_set_tx_done_callback(sx_uart_t *uart, void (*cb)(void *ctx), void *ctx)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    uart->txDoneCtx = ctx;
    uart->txDoneCb  = cb;
    __set_PRIMASK(primask);
}

void sx_uart_write_abort(sx_uart_t *uart)
{
    UART_HandleTypeDef *huart = (UART_HandleTypeDef *)uart->config->pDriver;
    if (uart->txBusy) { (void)HAL_UART_AbortTransmit(huart); uart->txBusy = 0U; }
}
#endif /* SPLC_MBM_ENABLE */

void sx_uart_set_rx_enabled(sx_uart_t *uart, bool enabled)
{
    UART_HandleTypeDef *huart = (UART_HandleTypeDef *)uart->config->pDriver;

    if (!enabled) {
        ATOMIC_CLEAR_BIT(huart->Instance->CR1, USART_CR1_RE);
        return;
    }

    /* Drop whatever the receiver shifted in while it was off or while the line
     * was driven by us, and any error flag that came with it, then switch the
     * receiver back on. */
    __HAL_UART_SEND_REQ(huart, UART_RXDATA_FLUSH_REQUEST);
    __HAL_UART_CLEAR_FLAG(huart, UART_CLEAR_OREF | UART_CLEAR_NEF |
                                 UART_CLEAR_PEF  | UART_CLEAR_FEF);
    ATOMIC_SET_BIT(huart->Instance->CR1, USART_CR1_RE);
}

int sx_uart_reconfigure(sx_uart_t *uart)
{
    UART_HandleTypeDef *huart = (UART_HandleTypeDef *)uart->config->pDriver;
    sx_uart_binding_t  *binding = find_binding(huart);
    const sx_uart_config_t *c = uart->config;

    if (binding == NULL || binding->uart != uart || c->baudrate == 0U || (c->bits != 7U && c->bits != 8U)) {
        return -1;
    }

    /* A parity bit is part of the word on the STM32: 8 data bits + parity = 9-bit word. */
    uint32_t word, parity, stop;
    switch (c->parity) {
    case 0:  parity = UART_PARITY_NONE; word = (c->bits == 8U) ? UART_WORDLENGTH_8B : UART_WORDLENGTH_7B; break;
    case 1:  parity = UART_PARITY_ODD;  word = (c->bits == 8U) ? UART_WORDLENGTH_9B : UART_WORDLENGTH_8B; break;
    case 2:  parity = UART_PARITY_EVEN; word = (c->bits == 8U) ? UART_WORDLENGTH_9B : UART_WORDLENGTH_8B; break;
    default: return -1;
    }
    switch (c->stopbits) {
    case 1:  stop = UART_STOPBITS_1; break;
    case 2:  stop = UART_STOPBITS_2; break;
    default: return -1;
    }

    /* Stop the armed receive first: HAL_UART_Init() resets RxState to READY and
     * would leave the interrupt receive half-armed. */
    (void)HAL_UART_AbortReceive(huart);

    huart->Init.BaudRate   = c->baudrate;
    huart->Init.WordLength = word;
    huart->Init.Parity     = parity;
    huart->Init.StopBits   = stop;
    if (HAL_UART_Init(huart) != HAL_OK) {
        arm_rx_byte(binding);     /* keep the old receiver alive if possible */
        return -2;
    }
    arm_rx_byte(binding);
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
    (void)splc_cqueue_send(&binding->uart->rxQueue, &binding->rx_byte);

    arm_rx_byte(binding);
}

#if SPLC_MBM_ENABLE
/* TC interrupt: last byte of HAL_UART_Transmit_IT() has left the shifter. The hook
 * (half-duplex DE release) runs first, then busy drops. */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    sx_uart_binding_t *binding = find_binding(huart);
    if (binding == NULL) { return; }
    sx_uart_t *uart = binding->uart;
    if (uart->txDoneCb != NULL) { uart->txDoneCb(uart->txDoneCtx); }
    uart->txBusy = 0U;
}
#endif /* SPLC_MBM_ENABLE */

/*
 * HAL aborts the interrupt receive on an overrun (UART_EndRxTransfer(): RXNE
 * and error interrupts are disabled, RxState goes back to READY) and never
 * starts it again, so after ONE overrun -- a noisy bus, or the engine not
 * servicing the queue for a while -- the UART would stay deaf forever. Re-arm
 * here. For the non-blocking errors (framing, noise, parity) the HAL keeps the
 * receive running and calls this callback as a notification only; then
 * HAL_UART_Receive_IT() just returns HAL_BUSY, which is harmless.
 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    sx_uart_binding_t *binding = find_binding(huart);
    if (binding == NULL) {
        return;
    }

    arm_rx_byte(binding);
}

#endif // STM32H5_PLATFORM