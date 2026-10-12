#include <string.h>

#include "splc_board.h"
#include "board_zigbee_io.h"
#include "board_config.h"
SPLC_BOARD_CHECK_SELECTED(BOARD_ZIGBEE_IO_4DI_4DO);

#include "splc_opt.h"
/* App link: this board talks to the App over USB-CDC and SimplePLC owns the USB
 * peripheral, through the optional port module port/usb/ (SPLC_PORT_USB_CDC = 1
 * in splcopts.h). The module registers the USB pipe with port/modbus_link; this
 * file only starts it, forwards the USB interrupt to it and asks the link for
 * its transport. It includes nothing from TinyUSB. */
#if !SPLC_PORT_USB_CDC
#error "board_zigbee_io.c talks to the App over USB-CDC: set SPLC_PORT_USB_CDC to 1 in splcopts.h"
#endif

#include "main.h"    /* CubeMX-generated: GPIOA/GPIOB, GPIO_PIN_x macros used
                       * by board_zigbee_io.h's DI*_PORT/DI*_PIN/DO*_PORT/
                       * DO*_PIN definitions -- board_zigbee_io.h does not
                       * include this itself, so this .c must. */
#include "usart.h"   /* CubeMX-generated: extern UART_HandleTypeDef hlpuart1 */

#include "sx_gpio.h"
#include "sx_uart.h"
#include "splc_usb_cdc.h"   /* optional USB-CDC port module: splc_usb_cdc_init(), splc_usb_irq() */
#include "modbus_link.h"    /* splc_link_get_transport() */
#include "splc_log.h"
#include "logger.h"     /* logger_init(): this board always uses the built-in logger */
#if !SPLC_LOGGER_BUILTIN
#error "board_zigbee_io.c initialises the built-in logger: SPLC_LOGGER_BUILTIN must be 1"
#endif
#include "sx_os.h"

#include "plc_io.h"
#include "plc_tag.h"        /* tag_di_base_index()/tag_do_base_index() */
#include "plc_device.h"
#include "plc_rule.h"       /* MAX_RULES */
#include "plc_modbus_cfg.h" /* g_device_descriptor / g_device_resource_info */
#include "plc_rtc.h"        /* g_rtc_caps */
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

/*
 * This SKU's tag layout: 4 DI, 4 DO, 0 AI, plus the fixed 32/32/32/8
 * VFLAG/VREG/VREG_RETAIN/COUNTER internal ranges every SKU gets per
 * docs/SimplePLC_App_MCU_Structs_v1.9_Self_Describing_Profile.md section
 * 5.1. This is the single source of truth for this board's tag counts --
 * board_get_tag_layout() (feeds Layer 2's tag_table_load_from_flash()) and
 * board_device_info_init() below both read from this same struct, so they
 * can never drift apart the way the old hardcoded 8/8/4 in
 * board_device_info_init() once did (see that function's doc-comment).
 */
static const SPLC_TagLayout s_tag_layout = {
    .di_count          = ZIGBEE_IO_DI_NUM,
    .do_count          = ZIGBEE_IO_DO_NUM,
    .ai_count          = ZIGBEE_IO_AI_NUM,
    .vflag_count       = ZIGBEE_IO_VFLAG_NUM,
    .vreg_count        = ZIGBEE_IO_VREG_NUM,
    .vreg_retain_count = ZIGBEE_IO_VREG_RETAIN_NUM,
    .counter_count     = ZIGBEE_IO_COUNTER_NUM,
};
SPLC_BOARD_CHECK_TAG_COUNT(SPLC_TAG_LAYOUT_TOTAL(
    ZIGBEE_IO_DI_NUM, ZIGBEE_IO_DO_NUM, ZIGBEE_IO_AI_NUM, ZIGBEE_IO_VFLAG_NUM,
    ZIGBEE_IO_VREG_NUM, ZIGBEE_IO_VREG_RETAIN_NUM, ZIGBEE_IO_COUNTER_NUM));

SPLC_TagLayout board_get_tag_layout(void)
{
    return s_tag_layout;
}

typedef struct {
    sx_gpio_pin_t di_pins[ZIGBEE_IO_DI_NUM];
    sx_gpio_pin_t do_pins[ZIGBEE_IO_DO_NUM];

    sx_uart_t         log_uart;
    sx_uart_config_t  log_uart_cfg;
} zigbee_io_board_t;

static zigbee_io_board_t s_board;

/* UART RX byte-buffer for the log channel -- sized generously for
 * logger_init()'s printf-style formatting; not yet tuned against real
 * message lengths. Same static-storage/no-malloc pattern as sx_uart_t
 * expects (components/uart/sx_uart.h doc-comment). */
static uint8_t s_log_rx_buf[128];
static uint8_t s_log_tx_buf[128];

/* True once splc_usb_cdc_init() has registered the App link and started USB. */
static bool s_usb_ok;

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

    /*
     * tag_di_base_index()/tag_do_base_index() (plc_tag.h, Layer 2) are used
     * here instead of board_tag_define.h's TAG_DI0/TAG_DO0 macros on
     * purpose: those macros are hand-computed at compile time from the
     * SAME numbers s_tag_layout below states, so the two could silently
     * drift apart if someone edited s_tag_layout without also updating
     * board_tag_define.h (e.g. changing di_count would shift where DO
     * actually starts, but the DO macro would stay at its old value --
     * no compile error, a silently wrong tag_idx at runtime). Calling the
     * getters instead means there is exactly one source of truth
     * (s_tag_layout, via tag_table_load_from_flash()) for where each group
     * starts; it cannot go out of sync with itself.
     */
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
 * Resource counts are read directly from s_tag_layout (this board's real
 * 4DI/4DO/0AI wiring) -- NOT a fixed Remote-IO-family 8DI/8DO/4AI layout.
 * Earlier versions of this function hardcoded di_count=8/do_count=8/
 * ai_count=4/runtime_tag_count=124 here to match the old Layer 2 default
 * (core/plc_tag/plc_tag_def.h, now removed), on the theory that this board
 * is "a Remote I/O family board first, reporting the family's full
 * resource profile" -- but that hardcoded copy had already drifted from
 * the values actually used a few lines below (4/4/0), which was itself a
 * live bug: the App would have seen 8/8/4 in DEVICE_RESOURCE_INFO while
 * only 4+4 channels were ever actually registered/scanned. Deriving from
 * s_tag_layout (the same struct board_get_tag_layout() returns, which
 * Layer 2's tag_table_load_from_flash() also builds g_tag_table[] from)
 * makes this a single source of truth -- it cannot drift again.
 * device_variant is a separate, purely identity/diagnostic field
 * (docs/SimplePLC_App_MCU_Structs_v2.0_Self_Describing_Profile.md section
 * 3.1: device_variant is identity only, it does not drive the App's
 * resource layout) -- it is set to SPLC_REMOTE_IO_VARIANT_4DI_4DO (3) so
 * App-side diagnostics can tell this board apart from a fully-populated
 * 8DI_8DO_4AI unit, without that value affecting how the App builds its
 * resource/tag catalog (that always comes from DEVICE_RESOURCE_INFO alone).
 *
 * hw_version and fw_version are not yet backed by any project-wide version
 * source (no VERSION file / CMake variable found) -- hardcoded to 1.0.0
 * here as a starting point; revisit once such a source exists so this does
 * not silently go stale across firmware builds.
 *
 * protocol_version = 2 and rule_format_version = 7: fixed values mandated
 * by Wire Profile V2.0 (docs/SimplePLC_App_MCU_Structs_v2.0_Self_
 * Describing_Profile.md section 3.1 and Golden Vector GV-001), NOT derived
 * from anything -- unlike hw/fw version above, these are protocol contract
 * numbers, not this board's own version. rule_format_version = 7 bumps
 * from 1 to signal the V2.0 wire contract to the App even though
 * SPLC_RuleRecord itself is still the same 32-byte layout (no field
 * changes) -- see handoff.md section 2.1 decision #1 for why
 * protocol_version is 2 and not 1 (the two source docs briefly disagreed).
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

    /*
     * Clock hardware of THIS board, reported to the Host in the RTC block's
     * status_flags (0x0813). Zigbee-IO uses the STM32's internal RTC on
     * LSI: no RTC IC, no 32.768 kHz crystal, no battery. A board with a
     * real RTC chip/crystal sets hw_present = true.
     */
    g_rtc_caps.hw_present = false;
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
    log_info(TAG, "Zigbee-IO board init start");
}

static void board_usb_init(void)
{
    /* Registers the USB-CDC pipe with the App link and starts TinyUSB. A
     * failure is logged and the board still runs (rules, retain, I/O); only the
     * App cannot reach it, and board_get_modbus_transport() then hands out an
     * idle transport. */
    splc_link_err_t err = splc_usb_cdc_init();
    s_usb_ok = (err == SPLC_LINK_OK);
    if (!s_usb_ok) {
        log_error(TAG, "USB-CDC App link init FAILED (%d)", (int)err);
    }
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
        log_info(TAG, "DI/DO registered (%d DI, %d DO)",
                 ZIGBEE_IO_DI_NUM, ZIGBEE_IO_DO_NUM);
    } else {
        log_error(TAG, "DI/DO registration: %d of %d channel(s) FAILED -- "
                       "those pins are NOT scanned", s_di_do_register_failures,
                       ZIGBEE_IO_DI_NUM + ZIGBEE_IO_DO_NUM);
    }

    board_usb_init();
    if (s_usb_ok) {
        log_info(TAG, "USB CDC initialized");

        /* Let the host enumerate the device before the engine starts scanning. */
        uint32_t t0 = HAL_GetTick();
        while ((HAL_GetTick() - t0) < 500U) {
            splc_usb_cdc_process();
            sx_os_yield_wait();   /* no-op bare-metal; 1-tick sleep under FreeRTOS */
        }
    }
}

/* Transport handed to plc_modbus_cfg when no App link exists: no byte ever
 * arrives and everything written is swallowed, so the Modbus server stays idle
 * instead of jumping through a NULL read pointer. */
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
    /* Zigbee-IO's App<->MCU config channel is USB-CDC: board_usb_init() (run by
     * board_hw_init(), which board.h guarantees happens before this function)
     * registered the USB pipe with the App link, so the transport is the
     * link's. */
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

/* USB interrupt: forwarded to the USB-CDC port module (TinyUSB stays inside it). */
void USB_DRD_FS_IRQHandler(void)
{
    splc_usb_irq();
}