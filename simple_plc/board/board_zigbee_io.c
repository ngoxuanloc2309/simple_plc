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

    for (int i = 0; i < 4; i++) {
        sx_gpio_init(&s_board.di_pins[i], SX_GPIO_LOW);
        plc_io_register_di((uint16_t)(TAG_DI0 + i), &s_board.di_pins[i]);
    }

    for (int i = 0; i < 4; i++) {
        sx_gpio_init(&s_board.do_pins[i], SX_GPIO_LOW);
        plc_io_register_do((uint16_t)(TAG_DO0 + i), &s_board.do_pins[i]);
    }
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
    /* Log UART first so board_log_write() is available for anything
     * that follows (mirrors sx_board_init()'s ordering in SynaptiX_FDK's
     * WS_v1 reference implementation). */
    board_log_uart_init();

    board_di_do_init();
    log_info(TAG, "DI/DO registered (4 DI, 4 DO)");

    board_usb_init();
    log_info(TAG, "USB CDC initialized");

    /* Pump tud_task() in a tight loop for ~300 ms right after
     * tusb_init() returns, instead of relying solely on the first
     * modbus_config_service() call in the 10 ms scan loop (main.c).
     *
     * Root-caused empirically, not from a known TinyUSB/errata
     * citation: without this pump, the host reliably reported "Unknown
     * USB Device" (Windows Device Manager). With this pump, the device
     * enumerates correctly within ~2 s of boot. The threshold was
     * narrowed by testing: 100 ms was NOT enough (enumeration still
     * failed), 300 ms IS enough (confirmed working), so 300 ms is used
     * here with no further safety margin added yet -- if enumeration
     * ever starts failing intermittently again (e.g. on a different
     * host controller/hub, or a marginal board), try raising this
     * first before looking elsewhere.
     *
     * Likely explanation (not fully confirmed): USB_DRD_FS's bus reset
     * handling and the first few control transfers of enumeration
     * happen in a tight back-and-forth that is sensitive to how
     * promptly tud_task() is called after each IRQ -- see
     * dcd_int_handler()/handle_bus_reset() in TinyUSB's
     * dcd_stm32_fsdev.c. The normal 10 ms scan-loop cadence
     * (modbus_config_service() -> transport->process() ->
     * sx_usb_tiny_process() -> tud_task()) may simply be too coarse
     * for that specific window right after dcd_init()/dcd_connect(),
     * even though 10 ms is fine once the device is already
     * enumerated/mounted and only steady-state CDC traffic is
     * involved. This has not been confirmed with a USB protocol
     * analyzer (see docs/architecture.md or ask before assuming this
     * reasoning is authoritative) -- treat it as the best available
     * explanation, not a verified root cause. */
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
    return modbus_transport_usb_create(&s_board.usb);
}

void USB_DRD_FS_IRQHandler(void)
{
    // log_debug(TAG, "USB IRQ fired");
    tud_int_handler(0);
}

