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
#include "plc_device.h"
#include "plc_rule.h"       /* MAX_RULES */
#include "plc_modbus_cfg.h" /* g_device_descriptor / g_device_resource_info */
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

/*
 * Populates g_device_descriptor / g_device_resource_info (owned by
 * services/plc_modbus_cfg/plc_modbus_cfg.c, Layer 3) with this SKU's real
 * identity/resource numbers, per plc_modbus_cfg.h's doc comment: "Layer 4's
 * board init needs to write into these once at boot". Without this call
 * both structs stay all-zero (static storage), so the App would read
 * device_class == SPLC_DEVICE_CLASS_UNKNOWN (0) at DEVICE_DESCRIPTOR
 * (0x0000) right after connecting and reject the device outright -- this
 * was reproduced against real hardware (App error: "Device class 0x0000
 * is not supported").
 *
 * Resource counts (di_count=8, do_count=8, ai_count=4, runtime_tag_count=
 * 124) intentionally follow plc_tag_def.h's Remote I/O 8DI/8DO/4AI layout,
 * NOT this board's actually-wired 4DI/4DO/0AI hardware (see
 * board_di_do_init() above, which only registers 4+4 channels) -- per
 * project decision: this board is a Remote I/O family board first, its
 * physical wiring is a partial population of that family's tag layout, not
 * a different resource profile. device_variant is a separate, purely
 * identity/diagnostic field (docs/SimplePLC_App_MCU_Structs_v1.9_
 * Self_Describing_Profile.md section 1: "ProductVariant chi la identity,
 * khong quyet dinh resource layout trong App") -- it is set to the new
 * SPLC_REMOTE_IO_VARIANT_4DI_4DO (3) so App-side diagnostics can tell this
 * board apart from a fully-populated 8DI_8DO_4AI unit, without that value
 * affecting how the App builds its resource/tag catalog (that always comes
 * from DEVICE_RESOURCE_INFO alone, per section 8.6).
 *
 * hw_version, fw_version and rule_format_version are not yet backed by any
 * project-wide version source (no VERSION file / CMake variable found) --
 * hardcoded to 1.0.0 (and rule_format_version = 1) here as a starting
 * point; revisit once such a source exists so this does not silently go
 * stale across firmware builds.
 */
static void board_device_info_init(void)
{
    g_device_descriptor.device_class        = SPLC_DEVICE_CLASS_REMOTE_IO;
    g_device_descriptor.device_variant      = SPLC_REMOTE_IO_VARIANT_4DI_4DO;

    g_device_descriptor.hw_version_major    = 1;
    g_device_descriptor.hw_version_minor    = 0;
    g_device_descriptor.hw_version_patch    = 0;

    g_device_descriptor.fw_version_major    = 1;
    g_device_descriptor.fw_version_minor    = 0;
    g_device_descriptor.fw_version_patch    = 0;

    g_device_descriptor.protocol_version    = 1;
    g_device_descriptor.rule_format_version = 1;

    g_device_resource_info.wire_profile         = SPLC_WIRE_PROFILE_V1;
    g_device_resource_info.max_rules            = MAX_RULES;
    g_device_resource_info.runtime_tag_count    = 124; /* DI+DO+AI+VFLAG+VREG+VREG_RETAIN+COUNTER, see plc_tag_def.h */

    g_device_resource_info.di_count             = 8;
    g_device_resource_info.do_count             = 8;
    g_device_resource_info.ai_count             = 4;
    g_device_resource_info.vflag_count          = 32;
    g_device_resource_info.vreg_count           = 32;
    g_device_resource_info.vreg_retain_count    = 32;
    g_device_resource_info.counter_count        = 8;
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

    logger_init(LOGGER_INFO, board_log_write);
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

    /* Must run before board_get_modbus_transport()/plc_modbus_cfg_init()
     * (Layer 4, see plc_engine.c) ever answers a Modbus request -- the App
     * may read DEVICE_DESCRIPTOR the moment USB enumerates, so this cannot
     * be deferred past board_usb_init() below. Placed after
     * board_log_uart_init() only so the confirmation log line below is
     * actually emitted (logger_init() runs inside board_log_uart_init()). */
    board_device_info_init();
    log_info(TAG, "device descriptor/resource info populated (class=REMOTE_IO, variant=4DI_4DO)");

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