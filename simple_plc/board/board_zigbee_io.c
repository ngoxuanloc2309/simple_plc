#include <string.h>

#include "board.h"
#include "board_zigbee_io.h"

#include "main.h"    /* CubeMX-generated: GPIOA/GPIOB, GPIO_PIN_x macros used
                       * by board_zigbee_io.h's DI*_PORT/DI*_PIN/DO*_PORT/
                       * DO*_PIN definitions -- board_zigbee_io.h does not
                       * include this itself, so this .c must. */
#include "usart.h"   /* CubeMX-generated: extern UART_HandleTypeDef hlpuart1 */

#include "sx_gpio.h"
#include "sx_uart.h"
#include "sx_usb_cdc.h"
#include "modbus_usb.h"
#include "logger.h"
#include "tusb.h"    /* tud_int_handler (macro for dcd_int_handler), used by
                       * USB_DRD_FS_IRQHandler() below -- declared in
                       * libs/tinyusb/src/device/usbd.h, pulled in
                       * transitively via tusb.h. */

#include "plc_io.h"
#include "plc_tag_def.h"
#include "tim.h"
#include "stm32h5xx_hal.h"

/*
 * board_zigbee_io.c - Layer 4
 *
 * board_hw_init() for the Zigbee-IO SKU (4 DI / 4 DO / 0 AI, see
 * board_zigbee_io.h). Follows the same "one Board_t struct owns every
 * driver instance" pattern used by SynaptiX_FDK's sx_board.c (WS_v1
 * repo) -- static storage here, never malloc'd, per the project's
 * no-dynamic-heap rule.
 *
 * Only this .c is linked into a Zigbee-IO firmware image -- a different
 * SKU (e.g. future board_remoteio.c) provides its own board_hw_init()
 * instead; the two are never compiled into the same target.
 */

static const char *TAG = "BOARD_ZIGBEE_IO";

typedef struct {
    sx_gpio_pin_t di_pins[4];
    sx_gpio_pin_t do_pins[4];

    sx_uart_t         log_uart;
    sx_uart_config_t  log_uart_cfg;

    sx_usb_tiny_t        usb;
    sx_usb_tiny_config_t usb_cfg;
} zigbee_io_board_t;

static zigbee_io_board_t s_board;

/* UART RX byte-buffer for the log channel -- sized generously for
 * logger_init()'s printf-style formatting; not yet tuned against real
 * message lengths. Same static-storage/no-malloc pattern as sx_uart_t
 * expects (components/uart/sx_uart.h doc-comment). */
static uint8_t s_log_rx_buf[128];
static uint8_t s_log_tx_buf[128];

/* USB CDC RX/TX buffers -- caller-owned per sx_usb_cdc.h's doc-comment,
 * sized via board_zigbee_io.h's USB_RX_BUF_SIZE/USB_TX_BUF_SIZE. */
static uint8_t s_usb_rx_buf[USB_RX_BUF_SIZE];
static uint8_t s_usb_tx_buf[USB_TX_BUF_SIZE];

/*
 * logger_init() takes a p_log_func (void (*)(const char *s)) -- adapts
 * that to sx_uart_write() on the log UART. Blocking write (see
 * sx_uart_write()'s doc-comment); acceptable for a log channel, not used
 * for the Modbus/USB data path.
 */
static void board_log_write(const char *s)
{
    sx_uart_write(&s_board.log_uart, (const uint8_t *)s, (int)strlen(s), 100);
}

/* Number of plc_io_register_*() calls that failed in board_di_do_init(). */
static int s_di_do_register_failures;

static void board_di_do_init(void)
{
    s_board.di_pins[0] = (sx_gpio_pin_t){ .port = DI0_PORT, .pin = DI0_PIN, .mode = SX_GPIO_MODE_INPUT };
    s_board.di_pins[1] = (sx_gpio_pin_t){ .port = DI1_PORT, .pin = DI1_PIN, .mode = SX_GPIO_MODE_INPUT };
    s_board.di_pins[2] = (sx_gpio_pin_t){ .port = DI2_PORT, .pin = DI2_PIN, .mode = SX_GPIO_MODE_INPUT };
    s_board.di_pins[3] = (sx_gpio_pin_t){ .port = DI3_PORT, .pin = DI3_PIN, .mode = SX_GPIO_MODE_INPUT };

    s_board.do_pins[0] = (sx_gpio_pin_t){ .port = DO0_PORT, .pin = DO0_PIN, .mode = SX_GPIO_MODE_OUTPUT_PP };
    s_board.do_pins[1] = (sx_gpio_pin_t){ .port = DO1_PORT, .pin = DO1_PIN, .mode = SX_GPIO_MODE_OUTPUT_PP };
    s_board.do_pins[2] = (sx_gpio_pin_t){ .port = DO2_PORT, .pin = DO2_PIN, .mode = SX_GPIO_MODE_OUTPUT_PP };
    s_board.do_pins[3] = (sx_gpio_pin_t){ .port = DO3_PORT, .pin = DO3_PIN, .mode = SX_GPIO_MODE_OUTPUT_PP };

    /* plc_io_register_*() return false (and register nothing) when the
     * tag's kind in g_tag_table[] is wrong or the table is full. That used
     * to be ignored, so a failed registration was completely silent: the
     * pin was configured but input_scan()/output_scan() never touched it.
     * Count and report failures so this can never hide again. */
    int reg_fail = 0;

    for (int i = 0; i < 4; i++) {
        sx_gpio_init(&s_board.di_pins[i], SX_GPIO_LOW);
        if (!plc_io_register_di((uint16_t)(TAG_DI0 + i), &s_board.di_pins[i])) {
            log_error(TAG, "register DI%d FAILED (tag kind != TAG_DI?)", i);
            reg_fail++;
        }
    }

    for (int i = 0; i < 4; i++) {
        sx_gpio_init(&s_board.do_pins[i], SX_GPIO_LOW);
        if (!plc_io_register_do((uint16_t)(TAG_DO0 + i), &s_board.do_pins[i])) {
            log_error(TAG, "register DO%d FAILED (tag kind != TAG_DO?)", i);
            reg_fail++;
        }
    }

    s_di_do_register_failures = reg_fail;
}

static void board_log_uart_init(void)
{
    s_board.log_uart_cfg.pDriver  = &UART_LOG;
    s_board.log_uart_cfg.baudrate = 115200;
    s_board.log_uart_cfg.bits     = 8;
    s_board.log_uart_cfg.parity   = 0;
    s_board.log_uart_cfg.stopbits = 1;

    s_board.log_uart.rxBuffer     = s_log_rx_buf;
    s_board.log_uart.rxBufferSize = (int)sizeof(s_log_rx_buf);
    s_board.log_uart.txBuffer     = s_log_tx_buf;
    s_board.log_uart.txBufferSize = (int)sizeof(s_log_tx_buf);

    sx_uart_init(&s_board.log_uart, &s_board.log_uart_cfg);

    logger_init(LOGGER_DEBUG, board_log_write);
    log_info(TAG, "Zigbee-IO board init start");
}

static void board_usb_init(void)
{
    s_board.usb_cfg.rx_buf_size = USB_RX_BUF_SIZE;
    s_board.usb_cfg.tx_buf_size = USB_TX_BUF_SIZE;

    s_board.usb.rxBuffer = s_usb_rx_buf;
    s_board.usb.txBuffer = s_usb_tx_buf;

    sx_usb_tiny_init(&s_board.usb, &s_board.usb_cfg);
}

void board_hw_init(void)
{
    board_log_uart_init();

    board_di_do_init();
    if (s_di_do_register_failures == 0) {
        log_info(TAG, "DI/DO registered (4 DI, 4 DO)");
    } else {
        log_error(TAG, "DI/DO registration: %d of 8 channel(s) FAILED -- "
                       "those pins are NOT scanned", s_di_do_register_failures);
    }

    board_usb_init();
    log_info(TAG, "USB CDC initialized");

    {
        uint32_t t0 = HAL_GetTick();
        while ((HAL_GetTick() - t0) < 500U) {
            sx_usb_tiny_process(&s_board.usb);
        }
    }
}

modbus_transport_t board_get_modbus_transport(void)
{
    /* Zigbee-IO's App<->MCU config channel is USB-CDC (see
     * docs/architecture.md section 0: USB, not RS485, for App<->MCU on
     * every SKU) -- s_board.usb was already initialized by
     * board_usb_init() above, which board_hw_init() guarantees ran
     * before this function is ever called (see board.h's doc-comment on
     * calling order). */
    return modbus_transport_usb_create(&s_board.usb, MODBUS_UNIT_ID);
}

void USB_DRD_FS_IRQHandler(void)
{
    tud_int_handler(0);
}