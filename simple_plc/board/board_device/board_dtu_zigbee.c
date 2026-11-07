#include <string.h>

#include "splc_board.h"
#include "board_dtu_zigbee.h"
#include "board_config.h"
SPLC_BOARD_CHECK_SELECTED(BOARD_DTU_ZIGBEE);

#include "splc_opt.h"

#include "modbus_link.h"    /* splc_link_get_transport() */
#include "splc_log.h"
#if SPLC_LOGGER_BUILTIN
#error "DTU Zigbee: the HOST owns the logger (and every UART): set SPLC_LOGGER_BUILTIN 0. SimplePLC's log goes to the host's log_func()."
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

/*
 * board_dtu_zigbee.c - Layer 4
 *
 * board_hw_init() for the DTU Zigbee SKU (see board_dtu_zigbee.h): no local I/O, three RS485
 * ports, one Zigbee module. SimplePLC is a LIBRARY here: it owns the logic, never the hardware.
 * The host firmware owns the logger, every UART (RS485, Zigbee) and USB; this file touches no
 * peripheral. What the host/product port plugs in, BEFORE plc_engine_init():
 *   - RS485 byte ports : splc_dtu_set_rs485_port()  (this file, forwarded to plc_mbm)
 *   - Zigbee byte pipe : splc_zigbee_register()     (plc_zigbee.h)
 *   - App link         : splc_link_register()       (modbus_link.h)
 * Same split as board_ethernet_wifi_gateway.c.
 *
 * USB: this file does NOT touch USB (no TinyUSB, no USB_DRD_FS_IRQHandler). The host firmware
 * owns the USB peripheral (handoff decision 15); the App link is a byte pipe the host/product
 * port registers with splc_link_register() BEFORE plc_engine_init() (see board_get_modbus_transport()).
 *
 * Needs nothing from the CubeMX project (never edited from here, handoff decision 9).
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

    /* Clock: with SPLC_RTC_EXTERNAL = 0 the library clock is the STM32 internal RTC on LSI
     * (decision 2): no RTC IC, no battery backing is reported. With SPLC_RTC_EXTERNAL = 1 the
     * product's RTC port registers the clock and sets hw_present itself; this board must not
     * overwrite it. */
#if !SPLC_RTC_EXTERNAL
    g_rtc_caps.hw_present = false;
#endif
}

/* ---- RS485 ports (MB_READ / MB_WRITE master, plc_mbs slave) ----------------------------- */
#if SPLC_MBM_ENABLE
#if SPLC_MBM_PORT_COUNT > 3
#error "DTU Zigbee has THREE RS-485 ports: set SPLC_MBM_PORT_COUNT 1..3."
#endif

/* Byte ports the host registered. Index = the App's port number - 1 (0x1400 table). The struct
 * is copied; port->ctx stays the host's. */
static modbus_serial_port_t s_rs485_port[SPLC_MBM_PORT_COUNT];
static plc_mbm_configure_fn s_rs485_configure[SPLC_MBM_PORT_COUNT];
static bool                 s_rs485_set[SPLC_MBM_PORT_COUNT];

bool splc_dtu_set_rs485_port(uint8_t port_index, const modbus_serial_port_t *port,
                             plc_mbm_configure_fn configure)
{
    if (port == NULL || port_index >= (uint8_t)SPLC_MBM_PORT_COUNT) {
        log_error(TAG, "set_rs485_port rejected (index %u)", (unsigned)port_index);
        return false;
    }
    s_rs485_port[port_index]      = *port;
    s_rs485_configure[port_index] = configure;
    s_rs485_set[port_index]       = true;
    return true;
}

static void board_mbm_init(void)
{
    for (uint8_t i = 0; i < (uint8_t)SPLC_MBM_PORT_COUNT; i++) {
        if (!s_rs485_set[i]) {
            log_warn(TAG, "RS485 port %u not registered (splc_dtu_set_rs485_port): its blocks report err_code 1",
                     (unsigned)(i + 1U));
            continue;
        }
        splc_mbm_set_rs485_port(i, &s_rs485_port[i], s_rs485_configure[i]);   /* copied */
        log_info(TAG, "RS485 port %u registered by the host, %lu baud%s", (unsigned)(i + 1U),
                 (unsigned long)s_rs485_port[i].baudrate,
                 (s_rs485_configure[i] != NULL) ? "" : " (line settings from the App ignored)");
    }
}
#endif /* SPLC_MBM_ENABLE */

/* ---- board init ----------------------------------------------------------------------- */
void board_hw_init(void)
{
    log_info(TAG, "DTU Zigbee board init start (host owns logger, UARTs and USB)");

    /* Before the App can read DEVICE_DESCRIPTOR (it does so as soon as the link is up). */
    board_device_info_init();
    log_info(TAG, "device descriptor/resource info populated (class=GATEWAY, no DI/DO)");

#if SPLC_MBM_ENABLE
    board_mbm_init();
#endif
    /* Zigbee: nothing here. The host/product port calls splc_zigbee_register() before
     * plc_engine_init(); the library declares the capability from then on. */
#if SPLC_INDICATOR_ENABLE
    splc_indicators_port_init();   /* 3 LEDs (after the Zigbee registration: it reads its state) */
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