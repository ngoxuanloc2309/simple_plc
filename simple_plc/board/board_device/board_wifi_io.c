#include <string.h>

#include "splc_board.h"
#include "board_wifi_io.h"
#include "board_config.h"
SPLC_BOARD_CHECK_SELECTED(BOARD_WIFI_IO);

#include "splc_opt.h"
/*
 * App link of this board: two ways to reach the App, picked by SPLC_PORT_USB_CDC
 * in splcopts.h; either way the core only sees the byte pipe of port/modbus_link.
 *   SPLC_PORT_USB_CDC = 1 : USB-CDC (CubeMX USB_DRD_FS, PA11/PA12). SimplePLC owns
 *                           the USB peripheral through its own port module
 *                           port/usb/ (TinyUSB), which registers the pipe itself.
 *                           This file starts it and forwards the USB interrupt.
 *                           It includes nothing from TinyUSB.
 *   SPLC_PORT_USB_CDC = 0 : RS485 Modbus RTU slave on USART1 (DE = PA15, active
 *                           high). rs485_output/rs485_pump are written right here.
 * Either way the Modbus master (MB_READ / MB_WRITE) is not built on this board
 * yet: USART1 is the only RS485 port and its master glue is not written.
 */
#if SPLC_MBM_ENABLE || SPLC_MBW_ENABLE
#error "board_wifi_io.c: MB_READ/MB_WRITE glue is not written for this board; set SPLC_MBM_ENABLE 0 and SPLC_MBW_ENABLE 0"
#endif

#include "main.h"    /* CubeMX-generated: GPIOx, GPIO_PIN_x used by board_wifi_io.h */
#include "usart.h"   /* CubeMX-generated: hlpuart1, huart1 */

#include "sx_gpio.h"
#include "sx_uart.h"
#if SPLC_PORT_USB_CDC
#include "splc_usb_cdc.h"   /* optional USB-CDC port module: splc_usb_cdc_init(), splc_usb_irq() */
#endif
#include "modbus_link.h"    /* splc_link_register(), splc_link_get_transport() */
#include "splc_log.h"
#include "logger.h"         /* logger_init(): this board always uses the built-in logger */
#if !SPLC_LOGGER_BUILTIN
#error "board_wifi_io.c initialises the built-in logger: SPLC_LOGGER_BUILTIN must be 1"
#endif
#include "sx_os.h"

#include "plc_io.h"
#include "plc_tag.h"        /* tag_di_base_index()/tag_do_base_index() */
#include "plc_device.h"
#include "plc_rule.h"       /* MAX_RULES */
#include "plc_modbus_cfg.h" /* g_device_descriptor / g_device_resource_info */
#include "plc_rtc.h"        /* g_rtc_caps */
#include "stm32h5xx_hal.h"

/*
 * board_wifi_io.c - Layer 4
 *
 * board_hw_init() for the Wi-Fi IO SKU (4 DI / 4 DO / 0 AI, see
 * board_wifi_io.h). Modelled on board_zigbee_io.c (RS485 App link variant).
 * Not done here (separate steps): the external RTC hooks (plc_rtc_register)
 * and the Wi-Fi ops (plc_wifi_register) -- they belong to the product glue
 * that also owns the I2C1 / ST67 drivers.
 */

static const char *TAG = "BOARD_WIFI_IO";

#ifndef RS485_TX_TIMEOUT_MS
#define RS485_TX_TIMEOUT_MS   100U   /* longest frame: 256 bytes = ~22 ms at 115200 */
#endif

/* Single source of truth for this board's tag counts (see board_zigbee_io.c). */
static const SPLC_TagLayout s_tag_layout = {
    .di_count          = WIFI_IO_DI_NUM,
    .do_count          = WIFI_IO_DO_NUM,
    .ai_count          = WIFI_IO_AI_NUM,
    .vflag_count       = WIFI_IO_VFLAG_NUM,
    .vreg_count        = WIFI_IO_VREG_NUM,
    .vreg_retain_count = WIFI_IO_VREG_RETAIN_NUM,
    .counter_count     = WIFI_IO_COUNTER_NUM,
};
SPLC_BOARD_CHECK_TAG_COUNT(SPLC_TAG_LAYOUT_TOTAL(
    WIFI_IO_DI_NUM, WIFI_IO_DO_NUM, WIFI_IO_AI_NUM, WIFI_IO_VFLAG_NUM,
    WIFI_IO_VREG_NUM, WIFI_IO_VREG_RETAIN_NUM, WIFI_IO_COUNTER_NUM));

SPLC_TagLayout board_get_tag_layout(void)
{
    return s_tag_layout;
}

typedef struct {
    sx_gpio_pin_t di_pins[WIFI_IO_DI_NUM];
    sx_gpio_pin_t do_pins[WIFI_IO_DO_NUM];

    sx_uart_t         log_uart;
    sx_uart_config_t  log_uart_cfg;

#if !SPLC_PORT_USB_CDC
    sx_uart_t         app_uart;       /* App link over RS485 (USART1) */
    sx_uart_config_t  app_uart_cfg;
#endif
} wifi_io_board_t;

static wifi_io_board_t s_board;

static uint8_t s_log_rx_buf[128];
static uint8_t s_log_tx_buf[128];

#if !SPLC_PORT_USB_CDC
/* RX byte queue of the RS485 App link: must hold the longest request that can
 * arrive during one 10 ms scan (~115 bytes at 115200); FC16 is at most 256. */
static uint8_t s_app_rx_buf[256];
static uint8_t s_app_tx_buf[1];   /* sx_uart_write() is direct, no TX queue */
#endif

/* True once the App link has been registered. */
static bool s_link_ok;

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

    /* plc_io_register_*() return false when the tag's kind in g_tag_table[] is
     * wrong or the table is full: count and report, never fail silently. */
    int reg_fail = 0;

    /* Base indices come from the layout (single source of truth), not from
     * hand-computed TAG_xxx macros. */
    uint16_t di_base = tag_di_base_index();
    uint16_t do_base = tag_do_base_index();

    for (int i = 0; i < s_tag_layout.di_count; i++) {
        sx_gpio_init(&s_board.di_pins[i], SX_GPIO_LOW);
        if (!plc_io_register_di((uint16_t)(di_base + i), &s_board.di_pins[i])) {
            log_error(TAG, "register DI%d FAILED (tag kind != TAG_DI?)", i);
            reg_fail++;
        }
    }

    for (int i = 0; i < s_tag_layout.do_count; i++) {
        sx_gpio_init(&s_board.do_pins[i], SX_GPIO_LOW);
        if (!plc_io_register_do((uint16_t)(do_base + i), &s_board.do_pins[i])) {
            log_error(TAG, "register DO%d FAILED (tag kind != TAG_DO?)", i);
            reg_fail++;
        }
    }

    s_di_do_register_failures = reg_fail;
}

/*
 * Fills g_device_descriptor / g_device_resource_info (plc_modbus_cfg.c) with
 * this SKU's identity and resource counts, read from s_tag_layout so they can
 * never drift from the tag table. Without this call both stay all-zero and the
 * App rejects the device (device class 0). protocol_version = 2 and
 * rule_format_version = 7 are Wire Profile V2.0 contract numbers. hw/fw
 * versions are placeholders (no project-wide version source yet).
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

    g_device_descriptor.protocol_version    = 2;
    g_device_descriptor.rule_format_version = 7;

    g_device_resource_info.wire_profile         = SPLC_WIRE_PROFILE_V2;
    g_device_resource_info.max_rules            = MAX_RULES;
    g_device_resource_info.runtime_tag_count    =
        (uint16_t)(s_tag_layout.di_count + s_tag_layout.do_count +
                   s_tag_layout.ai_count + s_tag_layout.vflag_count +
                   s_tag_layout.vreg_count + s_tag_layout.vreg_retain_count +
                   s_tag_layout.counter_count);

    g_device_resource_info.di_count             = s_tag_layout.di_count;
    g_device_resource_info.do_count             = s_tag_layout.do_count;
    g_device_resource_info.ai_count             = s_tag_layout.ai_count;
    g_device_resource_info.vflag_count          = s_tag_layout.vflag_count;
    g_device_resource_info.vreg_count           = s_tag_layout.vreg_count;
    g_device_resource_info.vreg_retain_count    = s_tag_layout.vreg_retain_count;
    g_device_resource_info.counter_count        = s_tag_layout.counter_count;

    /* This board has an external RTC IC on I2C1 (decision 57): report the
     * clock hardware as present. (Time is only valid once the product glue
     * registers the RTC hooks, see SPLC_RTC_EXTERNAL.) */
    g_rtc_caps.hw_present = true;
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

    logger_init((LOGGING_LEVELS)SPLC_LOG_LEVEL, board_log_write);
    log_info(TAG, "Wi-Fi IO board init start");
}

#if SPLC_PORT_USB_CDC

static void board_app_link_init(void)
{
    /* Registers the USB-CDC pipe with the App link and starts TinyUSB. A failure
     * is logged and the board still runs (rules, retain, I/O); only the App
     * cannot reach it, and board_get_modbus_transport() then hands out an idle
     * transport. */
    splc_link_err_t err = splc_usb_cdc_init();
    s_link_ok = (err == SPLC_LINK_OK);
    if (!s_link_ok) {
        log_error(TAG, "USB-CDC App link init FAILED (%d)", (int)err);
    }
}

#else /* RS485 */

static void rs485_de(bool transmit)
{
    GPIO_PinState level = (transmit == (RS485_DE_ACTIVE_HIGH != 0)) ? GPIO_PIN_SET
                                                                    : GPIO_PIN_RESET;
    HAL_GPIO_WritePin(RS485_DE_PORT, RS485_DE_PIN, level);
}

/* Link output (core -> bus). Blocking: returns when the last stop bit has left
 * the UART. The receiver is off for the duration so the transceiver's echo
 * never reaches the RX queue. */
static int32_t rs485_output(const uint8_t *buf, uint16_t len, void *ctx)
{
    (void)ctx;

    sx_uart_set_rx_enabled(&s_board.app_uart, false);
    rs485_de(true);
    int rc = sx_uart_write(&s_board.app_uart, buf, (int)len, RS485_TX_TIMEOUT_MS);
    rs485_de(false);
    sx_uart_set_rx_enabled(&s_board.app_uart, true);

    return (rc == 0) ? (int32_t)len : -1;
}

/* Link input (bus -> core): hand every byte received since the last call to the
 * core. Runs in the engine context once per scan. */
static void rs485_pump(void *ctx)
{
    (void)ctx;

    uint8_t chunk[32];
    int n;
    while ((n = sx_uart_read(&s_board.app_uart, chunk, (int)sizeof(chunk))) > 0) {
        (void)splc_link_input(chunk, (uint16_t)n);   /* overflow is counted by the link */
    }
}

static void board_app_link_init(void)
{
    s_board.app_uart_cfg.pDriver  = &UART_RS485;   /* CubeMX USART1 setup */
    s_board.app_uart_cfg.baudrate = 115200;
    s_board.app_uart_cfg.bits     = 8;
    s_board.app_uart_cfg.parity   = 0;
    s_board.app_uart_cfg.stopbits = 1;

    s_board.app_uart.rxBuffer     = s_app_rx_buf;
    s_board.app_uart.rxBufferSize = (int)sizeof(s_app_rx_buf);
    s_board.app_uart.txBuffer     = s_app_tx_buf;
    s_board.app_uart.txBufferSize = (int)sizeof(s_app_tx_buf);

    rs485_de(false);   /* listen */
    sx_uart_init(&s_board.app_uart, &s_board.app_uart_cfg);

    static const splc_link_ops_t ops = {
        .ctx     = NULL,
        .output  = rs485_output,
        .process = rs485_pump,
        .kind    = MODBUS_TRANSPORT_KIND_RTU,
        .unit_id = MODBUS_UNIT_ID,
    };
    splc_link_err_t err = splc_link_register(&ops);
    s_link_ok = (err == SPLC_LINK_OK);
    if (!s_link_ok) {
        log_error(TAG, "RS485 App link register FAILED (%d)", (int)err);
    }
}

#endif /* SPLC_PORT_USB_CDC */

void board_hw_init(void)
{
    board_log_uart_init();

    /* Must run before the App can read DEVICE_DESCRIPTOR (the moment the link
     * is up). */
    board_device_info_init();
    log_info(TAG, "device descriptor/resource info populated (class=REMOTE_IO, variant=4DI_4DO)");

    board_di_do_init();
    if (s_di_do_register_failures == 0) {
        log_info(TAG, "DI/DO registered (%d DI, %d DO)", WIFI_IO_DI_NUM, WIFI_IO_DO_NUM);
    } else {
        log_error(TAG, "DI/DO registration: %d of %d channel(s) FAILED -- "
                       "those pins are NOT scanned", s_di_do_register_failures,
                       WIFI_IO_DI_NUM + WIFI_IO_DO_NUM);
    }

    board_app_link_init();
#if SPLC_PORT_USB_CDC
    if (s_link_ok) {
        log_info(TAG, "USB CDC initialized");

        /* Let the host enumerate the device before the engine starts scanning. */
        uint32_t t0 = HAL_GetTick();
        while ((HAL_GetTick() - t0) < 500U) {
            splc_usb_cdc_process();
            sx_os_yield_wait();   /* 1-tick sleep under FreeRTOS */
        }
    }
#else
    if (s_link_ok) {
        log_info(TAG, "UART/RS485 App link initialized (USART1, unit %d)", (int)MODBUS_UNIT_ID);
    }
#endif
}

/* Transport handed to plc_modbus_cfg when no App link exists: nothing arrives
 * and everything written is swallowed, so the Modbus server stays idle. */
static int32_t idle_read(uint8_t *buf, uint16_t count, int32_t timeout_ms, void *ctx)
{
    (void)buf; (void)count; (void)timeout_ms; (void)ctx;
    return 0;
}

static int32_t idle_write(const uint8_t *buf, uint16_t count, int32_t timeout_ms, void *ctx)
{
    (void)buf; (void)timeout_ms; (void)ctx;
    return (int32_t)count;
}

modbus_transport_t board_get_modbus_transport(void)
{
    modbus_transport_t t;
    if (splc_link_get_transport(&t)) {
        return t;
    }

    log_error(TAG, "no App link registered -- Modbus config server stays idle");
    t.ctx     = NULL;
    t.read    = idle_read;
    t.write   = idle_write;
    t.process = NULL;
    t.kind    = MODBUS_TRANSPORT_KIND_RTU;
    t.unit_id = MODBUS_UNIT_ID;
    return t;
}

#if SPLC_PORT_USB_CDC
/* USB interrupt: forwarded to the USB-CDC port module (TinyUSB stays inside it). */
void USB_DRD_FS_IRQHandler(void)
{
    splc_usb_irq();
}
#endif