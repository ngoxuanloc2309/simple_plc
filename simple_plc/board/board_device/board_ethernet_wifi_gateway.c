#include <string.h>

#include "board.h"
#include "board_ethernet_wifi_gateway.h"
#include "board_config.h"
SPLC_BOARD_CHECK_SELECTED(BOARD_GATEWAY);

#include "main.h"    /* CubeMX: USART1_DE_GPIO_Port / USART1_DE_Pin */
#include "usart.h"   /* CubeMX: extern UART_HandleTypeDef hlpuart1, huart1 */

#include "sx_gpio.h"
#include "sx_uart.h"
#include "logger.h"

#include "plc_tag.h"
#include "plc_device.h"
#include "plc_rule.h"        /* MAX_RULES */
#include "plc_modbus_cfg.h"  /* g_device_descriptor / g_device_resource_info */
#include "plc_rtc.h"         /* g_rtc_caps */
#include "plc_io.h"
#include "plc_io_remote.h"
#include "modbus_serial.h"

/*
 * board_ethernet_wifi_gateway.c - Layer 4
 *
 * Gateway with virtual DI/DO. Peripherals in use: log UART (UART_LOG) and
 * RS485 UART (UART_RS485) -- nothing else is initialised here. The DI/DO
 * tags have no pins: plc_io_remote moves them to the I/O boards listed in
 * GATEWAY_IO_NODES (board_ethernet_wifi_gateway.h) over RS485.
 *
 * No App<->device link (USB-CDC) is set up by this file: the transport it
 * hands to plc_modbus_cfg is a silent placeholder (no frame ever arrives), so
 * the Modbus config server stays idle. The Ethernet/WiFi link that will carry
 * configuration is added by replacing board_get_modbus_transport().
 */

static const char *TAG = "BOARD_GATEWAY";

/* --- tag layout: single source of truth, derived from GATEWAY_IO_NODES --- */
static const SPLC_TagLayout s_tag_layout = {
    .di_count          = GATEWAY_FAKE_IO_DI_NUM,
    .do_count          = GATEWAY_FAKE_IO_DO_NUM,
    .ai_count          = GATEWAY_AI_NUM,
    .vflag_count       = GATEWAY_VFLAG_NUM,
    .vreg_count        = GATEWAY_VREG_NUM,
    .vreg_retain_count = GATEWAY_VREG_RETAIN_NUM,
    .counter_count     = GATEWAY_COUNTER_NUM,
};
SPLC_BOARD_CHECK_TAG_COUNT(SPLC_TAG_LAYOUT_TOTAL(
    GATEWAY_FAKE_IO_DI_NUM, GATEWAY_FAKE_IO_DO_NUM, GATEWAY_AI_NUM,
    GATEWAY_VFLAG_NUM, GATEWAY_VREG_NUM, GATEWAY_VREG_RETAIN_NUM,
    GATEWAY_COUNTER_NUM));

/* Compile-time limits of the node table (each row = one I/O board). */
#define GW_ROW_CHECK_(unit, di, do_) \
    _Static_assert((unit) >= 1 && (unit) <= 247, \
                   "GATEWAY_IO_NODES: unit id must be 1..247"); \
    _Static_assert((di) <= PLC_IO_REMOTE_MAX_BITS && (do_) <= PLC_IO_REMOTE_MAX_BITS, \
                   "GATEWAY_IO_NODES: at most 16 DI and 16 DO per I/O board");
GATEWAY_IO_NODES(GW_ROW_CHECK_)
_Static_assert(GATEWAY_NODE_COUNT <= SPLC_REMOTE_MAX_NODES,
               "GATEWAY_IO_NODES has more rows than SPLC_REMOTE_MAX_NODES (splcopts.h)");
_Static_assert(GATEWAY_NODE_COUNT >= 1, "GATEWAY_IO_NODES must have at least one row");

SPLC_TagLayout board_get_tag_layout(void)
{
    return s_tag_layout;
}

/* --- peripherals ------------------------------------------------------- */

typedef struct {
    sx_uart_t           log_uart;
    sx_uart_config_t    log_uart_cfg;
    sx_uart_t           rs485_uart;
    sx_uart_config_t    rs485_uart_cfg;
    sx_gpio_pin_t       rs485_de;
    modbus_rtu_master_t rs485_master;
} gateway_board_t;

static gateway_board_t s_board;

static uint8_t s_log_rx_buf[128];
static uint8_t s_log_tx_buf[128];
/* RS485 RX: the longest answer is 37 bytes (16 registers); 128 leaves room
 * for a burst between two scan cycles. */
static uint8_t s_rs485_rx_buf[128];
static uint8_t s_rs485_tx_buf[64];

static void board_log_write(const char *s)
{
    sx_uart_write(&s_board.log_uart, (const uint8_t *)s, (int)strlen(s), 100);
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
    log_info(TAG, "Ethernet/WiFi gateway board init start");
}

static void board_rs485_init(void)
{
    s_board.rs485_uart_cfg.pDriver  = &UART_RS485;
    s_board.rs485_uart_cfg.baudrate = GATEWAY_RS485_BAUDRATE;
    s_board.rs485_uart_cfg.bits     = 8;
    s_board.rs485_uart_cfg.parity   = 0;
    s_board.rs485_uart_cfg.stopbits = 1;

    s_board.rs485_uart.rxBuffer     = s_rs485_rx_buf;
    s_board.rs485_uart.rxBufferSize = (int)sizeof(s_rs485_rx_buf);
    s_board.rs485_uart.txBuffer     = s_rs485_tx_buf;
    s_board.rs485_uart.txBufferSize = (int)sizeof(s_rs485_tx_buf);

    sx_uart_init(&s_board.rs485_uart, &s_board.rs485_uart_cfg);

    s_board.rs485_de = (sx_gpio_pin_t){
        .port = GATEWAY_RS485_DE_PORT,
        .pin  = GATEWAY_RS485_DE_PIN,
        .mode = SX_GPIO_MODE_OUTPUT_PP
    };
    sx_gpio_init(&s_board.rs485_de, SX_GPIO_LOW);   /* receive by default */

    modbus_rtu_master_init(&s_board.rs485_master, &s_board.rs485_uart, &s_board.rs485_de);
}

/* --- virtual DI/DO ----------------------------------------------------- */

/* One configuration entry per row of GATEWAY_IO_NODES. */
#define GW_NODE_ROW_(unit, di, do_) \
    { .master = &s_board.rs485_master, .unit_id = (unit), \
      .di_count = (di), .do_count = (do_), \
      .lease_ms = GATEWAY_NODE_LEASE_MS, .safe_value = GATEWAY_NODE_SAFE_VALUE },

static void board_virtual_io_init(void)
{
    const plc_io_remote_node_cfg_t nodes[GATEWAY_NODE_COUNT] = {
        GATEWAY_IO_NODES(GW_NODE_ROW_)
    };

    plc_io_remote_reset();

    /* Virtual tags follow the rows in order (see the header comment). The
     * base indices come from the Tag Table's real layout, not from macros. */
    uint16_t di_tag = tag_di_base_index();
    uint16_t do_tag = tag_do_base_index();
    int failures = 0;

    for (int i = 0; i < GATEWAY_NODE_COUNT; i++) {
        int node = plc_io_remote_add_node(&nodes[i]);
        if (node < 0) {
            log_error(TAG, "I/O board row %d (unit %u): add FAILED", i, nodes[i].unit_id);
            failures++;
            continue;
        }
        failures += plc_io_remote_map_node(node, di_tag, do_tag);
        log_info(TAG, "I/O board %d: unit %u, %u DI -> tags %u.., %u DO -> tags %u..",
                 node, nodes[i].unit_id, nodes[i].di_count, di_tag,
                 nodes[i].do_count, do_tag);
        di_tag = (uint16_t)(di_tag + nodes[i].di_count);
        do_tag = (uint16_t)(do_tag + nodes[i].do_count);
    }
    if (failures != 0) {
        log_error(TAG, "virtual I/O: %d mapping(s) FAILED -- those bits are NOT served",
                  failures);
    }
}

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

    /* Wire Profile V2.0 contract numbers (same as board_zigbee_io.c). */
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

    g_rtc_caps.hw_present = false;
}

void board_hw_init(void)
{
    board_log_uart_init();
    board_device_info_init();
    board_rs485_init();
    log_info(TAG, "RS485 master ready (%u baud)", (unsigned)GATEWAY_RS485_BAUDRATE);
    board_virtual_io_init();
}

/* --- App link: placeholder -------------------------------------------- */

static int32_t idle_read(uint8_t *buf, uint16_t count, int32_t timeout_ms, void *ctx)
{
    (void)buf; (void)count; (void)timeout_ms; (void)ctx;
    return 0;   /* nothing ever arrives */
}

static int32_t idle_write(const uint8_t *buf, uint16_t count, int32_t timeout_ms, void *ctx)
{
    (void)buf; (void)timeout_ms; (void)ctx;
    return (int32_t)count;
}

modbus_transport_t board_get_modbus_transport(void)
{
    modbus_transport_t t;
    t.ctx     = NULL;
    t.read    = idle_read;
    t.write   = idle_write;
    t.process = NULL;
    t.kind    = MODBUS_TRANSPORT_KIND_RTU;
    t.unit_id = MODBUS_UNIT_ID;
    return t;
}