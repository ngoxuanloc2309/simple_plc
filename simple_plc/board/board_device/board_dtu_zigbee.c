#include <string.h>

#include "splc_board.h"
#include "board_dtu_zigbee.h"
#include "board_config.h"
SPLC_BOARD_CHECK_SELECTED(BOARD_DTU_ZIGBEE);

#include "splc_opt.h"

#include "main.h"    /* CubeMX-generated: GPIO ports/pins used by board_dtu_zigbee.h */
#include "usart.h"   /* CubeMX-generated: huart1 / huart3 / huart7 / huart9 / huart10 */

#include "sx_uart.h"
#include "modbus_link.h"    /* splc_link_get_transport() */
#include "splc_log.h"
#include "logger.h"         /* logger_init(): this board uses the built-in logger */
#if !SPLC_LOGGER_BUILTIN
#error "board_dtu_zigbee.c initialises the built-in logger: SPLC_LOGGER_BUILTIN must be 1"
#endif
#if SPLC_PORT_USB_CDC
#error "DTU Zigbee: the HOST owns USB (TinyUSB, decision 15): set SPLC_PORT_USB_CDC 0. The App link is registered by the host/port with splc_link_register() before plc_engine_init()."
#endif

#include "plc_tag.h"
#include "plc_device.h"
#include "plc_rule.h"        /* MAX_RULES */
#include "plc_modbus_cfg.h"  /* g_device_descriptor / g_device_resource_info */
#include "plc_rtc.h"         /* g_rtc_caps */
#if SPLC_MBM_ENABLE
#include "plc_mbm.h"         /* splc_mbm_set_rs485_port() */
#include "modbus_serial.h"   /* modbus_serial_port_t */
#endif
#include "stm32h5xx_hal.h"

/*
 * board_dtu_zigbee.c - Layer 4
 *
 * board_hw_init() for the DTU Zigbee SKU (see board_dtu_zigbee.h): no local I/O, three RS485
 * ports, one Zigbee module. Same shape as board_zigbee_io.c (the board owns its UARTs), without
 * the DI/DO part.
 *
 * USB: this file does NOT touch USB (no TinyUSB, no USB_DRD_FS_IRQHandler). The host firmware
 * owns the USB peripheral (handoff decision 15); the App link is a byte pipe the host/product
 * port registers with splc_link_register() BEFORE plc_engine_init() (see board_get_modbus_transport()).
 *
 * Needs, in the CubeMX project (never edited from here, handoff decision 9):
 *   - USART1 / USART3 / UART7 / UART9 / USART10 initialised (MX_*_Init) with RX interrupt
 *     capable NVIC settings, as RF_Gateway.ioc already does.
 */

static const char *TAG = "BOARD_DTU_ZIGBEE";

/* ---- tag layout: single source of truth ---------------------------------------------- */
static const SPLC_TagLayout s_tag_layout = {
    .di_count          = DTU_ZIGBEE_DI_NUM,
    .do_count          = DTU_ZIGBEE_DO_NUM,
    .ai_count          = DTU_ZIGBEE_AI_NUM,
    .vflag_count       = DTU_ZIGBEE_VFLAG_NUM,
    .vreg_count        = DTU_ZIGBEE_VREG_NUM,
    .vreg_retain_count = DTU_ZIGBEE_VREG_RETAIN_NUM,
    .counter_count     = DTU_ZIGBEE_COUNTER_NUM,
};
SPLC_BOARD_CHECK_TAG_COUNT(SPLC_TAG_LAYOUT_TOTAL(
    DTU_ZIGBEE_DI_NUM, DTU_ZIGBEE_DO_NUM, DTU_ZIGBEE_AI_NUM, DTU_ZIGBEE_VFLAG_NUM,
    DTU_ZIGBEE_VREG_NUM, DTU_ZIGBEE_VREG_RETAIN_NUM, DTU_ZIGBEE_COUNTER_NUM));

SPLC_TagLayout board_get_tag_layout(void)
{
    return s_tag_layout;
}

/* ---- log UART (USART1) ---------------------------------------------------------------- */
static sx_uart_t        s_log_uart;
static sx_uart_config_t s_log_uart_cfg;
static uint8_t          s_log_rx_buf[128];
static uint8_t          s_log_tx_buf[128];

static void board_log_write(const char *s)
{
    sx_uart_write(&s_log_uart, (const uint8_t *)s, (int)strlen(s), 100);
}

static void board_log_uart_init(void)
{
    s_log_uart_cfg.pDriver  = &UART_LOG;
    s_log_uart_cfg.baudrate = 115200;
    s_log_uart_cfg.bits     = 8;
    s_log_uart_cfg.parity   = 0;
    s_log_uart_cfg.stopbits = 1;

    s_log_uart.rxBuffer     = s_log_rx_buf;
    s_log_uart.rxBufferSize = (int)sizeof(s_log_rx_buf);
    s_log_uart.txBuffer     = s_log_tx_buf;
    s_log_uart.txBufferSize = (int)sizeof(s_log_tx_buf);

    sx_uart_init(&s_log_uart, &s_log_uart_cfg);

    logger_init((LOGGING_LEVELS)SPLC_LOG_LEVEL, board_log_write);
    log_info(TAG, "DTU Zigbee board init start");
}

/* ---- device identity ------------------------------------------------------------------ */
/*
 * Fills g_device_descriptor / g_device_resource_info (see board_zigbee_io.c for why this must
 * happen before the App can read DEVICE_DESCRIPTOR). Class = GATEWAY, variant = RS485_ETH: the
 * library has no DTU variant and the wire enum is the App's contract, so none is invented here.
 * hw/fw versions are placeholders (1.0.0) like the other boards.
 */
static void board_device_info_init(void)
{
    g_device_descriptor.device_class        = SPLC_DEVICE_CLASS_GATEWAY;
    g_device_descriptor.device_variant      = SPLC_GATEWAY_VARIANT_RS485_ETH;

    g_device_descriptor.hw_version_major    = 1;
    g_device_descriptor.hw_version_minor    = 0;
    g_device_descriptor.hw_version_patch    = 0;
    g_device_descriptor.fw_version_major    = 1;
    g_device_descriptor.fw_version_minor    = 0;
    g_device_descriptor.fw_version_patch    = 0;

    /* Wire Profile V2.0 contract numbers (same as every SKU). */
    g_device_descriptor.protocol_version    = 2;
    g_device_descriptor.rule_format_version = 7;

    g_device_resource_info.wire_profile         = SPLC_WIRE_PROFILE_V2;
    g_device_resource_info.max_rules            = MAX_RULES;
    g_device_resource_info.runtime_tag_count    =
        (uint16_t)SPLC_TAG_LAYOUT_TOTAL(s_tag_layout.di_count, s_tag_layout.do_count,
                                        s_tag_layout.ai_count, s_tag_layout.vflag_count,
                                        s_tag_layout.vreg_count, s_tag_layout.vreg_retain_count,
                                        s_tag_layout.counter_count);
    g_device_resource_info.di_count             = s_tag_layout.di_count;
    g_device_resource_info.do_count             = s_tag_layout.do_count;
    g_device_resource_info.ai_count             = s_tag_layout.ai_count;
    g_device_resource_info.vflag_count          = s_tag_layout.vflag_count;
    g_device_resource_info.vreg_count           = s_tag_layout.vreg_count;
    g_device_resource_info.vreg_retain_count    = s_tag_layout.vreg_retain_count;
    g_device_resource_info.counter_count        = s_tag_layout.counter_count;

    /* The library clock is the STM32 internal RTC on LSI (decision 2): no RTC IC, no battery
     * backing is reported. The board does have a 32.768 kHz crystal (PC14/PC15) but it is not
     * used by the library's RTC. */
    g_rtc_caps.hw_present = false;
}

/* ---- RS485 ports (MB_READ / MB_WRITE master, plc_mbs slave) ----------------------------- */
#if SPLC_MBM_ENABLE

#if SPLC_MBM_PORT_COUNT > 3
#error "DTU Zigbee has THREE RS-485 ports: set SPLC_MBM_PORT_COUNT 1..3."
#endif

#define MBM_DEFAULT_BAUD  9600U

/* One block per port: its UART, DE pin, and byte queues. */
typedef struct {
    UART_HandleTypeDef *huart;
    GPIO_TypeDef       *de_port;
    uint16_t            de_pin;
    sx_uart_t           uart;
    sx_uart_config_t    cfg;
    uint8_t             rx_buf[256];   /* one full 125-register answer = 255 bytes */
    uint8_t             tx_buf[1];     /* unused: the frame lives in the master */
} rs485_port_t;

static rs485_port_t s_rs485[3] = {
    { .huart = &UART_485_PORT1, .de_port = RS485_DE_PORT1, .de_pin = RS485_DE_PIN1 },
    { .huart = &UART_485_PORT2, .de_port = RS485_DE_PORT2, .de_pin = RS485_DE_PIN2 },
    { .huart = &UART_485_PORT3, .de_port = RS485_DE_PORT3, .de_pin = RS485_DE_PIN3 },
};

static void rs485_de(const rs485_port_t *p, bool transmit)
{
    HAL_GPIO_WritePin(p->de_port, p->de_pin,
                      (transmit == (RS485_DE_ACTIVE_HIGH != 0)) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static int rs485_read(void *ctx, uint8_t *buf, uint16_t max_len)
{
    rs485_port_t *p = (rs485_port_t *)ctx;
    return sx_uart_read(&p->uart, buf, (int)max_len);
}

/* Receiver off, DE up, blocking send (returns after the last bit left the UART), DE down,
 * receiver on again. Switching the receiver back on flushes it, so the echo of our own frame
 * never reaches rs485_read(). Blocking like Zigbee-IO (decision 46): the scan stalls for the
 * frame time. */
static int rs485_write_start(void *ctx, const uint8_t *buf, uint16_t len)
{
    rs485_port_t *p = (rs485_port_t *)ctx;
    /* Worst case 12 bit times per character (start + 8 data + parity + 2 stop), plus margin. */
    const uint32_t baud = (p->cfg.baudrate != 0U) ? p->cfg.baudrate : MBM_DEFAULT_BAUD;
    const uint32_t timeout_ms = (((uint32_t)len * 12U * 1000U) / baud) + 20U;

    sx_uart_set_rx_enabled(&p->uart, false);
    rs485_de(p, true);
    const int rc = sx_uart_write(&p->uart, buf, (int)len, timeout_ms);
    rs485_de(p, false);
    sx_uart_set_rx_enabled(&p->uart, true);
    return (rc == 0) ? 0 : -1;
}

/* The frame is already out when rs485_write_start() returns. */
static bool rs485_tx_done(void *ctx)
{
    (void)ctx;
    return true;
}

static int rs485_configure(void *ctx, const plc_mbm_serial_t *cfg)
{
    rs485_port_t *p = (rs485_port_t *)ctx;
    p->cfg.baudrate = cfg->baud;
    p->cfg.bits     = cfg->data_bits;
    /* wire: 1 = even, 2 = odd (MBM-003); sx_uart: 1 = odd, 2 = even */
    p->cfg.parity   = (cfg->parity == PLC_MBM_PARITY_EVEN) ? 2U :
                      (cfg->parity == PLC_MBM_PARITY_ODD)  ? 1U : 0U;
    p->cfg.stopbits = cfg->stop_bits;
    return sx_uart_reconfigure(&p->uart);
}

static void board_mbm_init(void)
{
    for (uint8_t i = 0; i < (uint8_t)SPLC_MBM_PORT_COUNT; i++) {
        rs485_port_t *p = &s_rs485[i];

        p->cfg.pDriver  = p->huart;
        p->cfg.baudrate = MBM_DEFAULT_BAUD;
        p->cfg.bits     = 8;
        p->cfg.parity   = 0;
        p->cfg.stopbits = 1;
        p->uart.rxBuffer     = p->rx_buf;
        p->uart.rxBufferSize = (int)sizeof(p->rx_buf);
        p->uart.txBuffer     = p->tx_buf;
        p->uart.txBufferSize = (int)sizeof(p->tx_buf);

        rs485_de(p, false);   /* listen */
        sx_uart_init(&p->uart, &p->cfg);

        const modbus_serial_port_t port = {
            .read        = rs485_read,
            .write       = NULL,
            .set_tx      = NULL,
            .ctx         = p,
            .baudrate    = MBM_DEFAULT_BAUD,
            .write_start = rs485_write_start,
            .tx_done     = rs485_tx_done,
            .write_abort = NULL,
        };
        splc_mbm_set_rs485_port(i, &port, rs485_configure);   /* the struct is copied */
        log_info(TAG, "RS485 port %u registered, blocking TX, %lu baud until the App sets it",
                 (unsigned)(i + 1U), (unsigned long)MBM_DEFAULT_BAUD);
    }
}
#endif /* SPLC_MBM_ENABLE */

/* ---- board init ----------------------------------------------------------------------- */
void board_hw_init(void)
{
    board_log_uart_init();

    /* Before the App can read DEVICE_DESCRIPTOR (it does so as soon as the link is up). */
    board_device_info_init();
    log_info(TAG, "device descriptor/resource info populated (class=GATEWAY, no DI/DO)");

#if SPLC_MBM_ENABLE
    board_mbm_init();
#endif
#if SPLC_ZIGBEE_ENABLE
    splc_zigbee_port_init();       /* UART9 + module: product port splc_zigbee_port.c */
#endif
#if SPLC_INDICATOR_ENABLE
    splc_indicators_port_init();   /* 3 LEDs: product port splc_indicators_port.c (after the Zigbee port: it reads its state) */
#endif
}

/* Transport handed to plc_modbus_cfg when no App link exists: no byte ever arrives and
 * everything written is swallowed, so the Modbus server stays idle. */
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
    /* plc_engine_init() calls this AFTER splc_board_init(), as its last step before
     * plc_modbus_cfg_init(). The host/product port must therefore have called
     * splc_link_register() before plc_engine_init(); this board registers nothing itself.
     * If it did not, say so instead of running a device the App can never reach. */
    modbus_transport_t t;
    if (splc_link_get_transport(&t)) {
        t.kind = MODBUS_TRANSPORT_KIND_RTU;   /* framing is chosen here, not by the byte link */
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