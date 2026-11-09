#include "splc_zigbee_port.h"
#include "splc_opt.h"

#if SPLC_ZIGBEE_ENABLE

#include <stdint.h>
#include <stdbool.h>

#include "splc_log.h"
#include "usart.h"          /* CubeMX-generated: huart2 */
#include "sx_uart.h"
#include "modbus_serial.h"  /* modbus_serial_port_t (used by plc_zigbee.h) */
#include "plc_zigbee.h"     /* splc_zigbee_register(), splc_zigbee_get_config() */
#if SPLC_INDICATOR_ENABLE
#include "sx_time.h"        /* sx_get_tick_ms(): time of the last received byte */
#endif

/*
 * Zigbee byte pipe: USART2 (PA2 TX / PA3 RX) <-> DRF1609H in transparent mode. The module
 * has no RS-485 direction pin and no echo, so unlike USART1 there is no DE and no receiver
 * switching: read() drains the RX queue, write() is a plain blocking send.
 *
 * Line settings are the CubeMX ones (115200 8N1) and are NOT touched here: the module's own
 * UART (X9..X12) must be set to the same speed (its factory default is 38400, handoff 97e).
 * Both roles are declared; the library serves the Slave role with SLAVE-001 on this pipe
 * (plc_mbs) and uses it as a Master port for MB_READ / MB_WRITE (plc_mbm, raw port 4).
 * The module is not configured from here (no utils/zigbee call): PAN / channel / key written
 * to 0x1700 only reach the library until a driver for the INS commands exists.
 */

/* The UART the module is wired to. Override with -DSPLC_ZIGBEE_PORT_UART=<handle>. */
#ifndef SPLC_ZIGBEE_PORT_UART
#define SPLC_ZIGBEE_PORT_UART huart2
#endif

#define ZB_DEFAULT_BAUD      115200U
#define ZB_ERR_BAD_SLAVE_ID  1U     /* port-defined ERROR_CODE (0x1721): Slave ID the module eats */

static const char *TAG = "ZIGBEE_PORT";

static sx_uart_t        s_zb_uart;
static sx_uart_config_t s_zb_uart_cfg;
static uint8_t          s_zb_rx_buf[512];   /* one transparent packet is up to 269 bytes */
static uint8_t          s_zb_tx_buf[1];     /* unused: sx_uart_write() is direct */

#if SPLC_INDICATOR_ENABLE
static bool     s_zb_rx_seen;
static uint32_t s_zb_rx_tick;
#endif

static int zb_read(void *ctx, uint8_t *buf, uint16_t max_len)
{
    (void)ctx;
    const int n = sx_uart_read(&s_zb_uart, buf, (int)max_len);
#if SPLC_INDICATOR_ENABLE
    if (n > 0) {
        s_zb_rx_seen = true;
        s_zb_rx_tick = sx_get_tick_ms();
    }
#endif
    return n;
}

#if SPLC_INDICATOR_ENABLE
uint32_t splc_zigbee_port_ms_since_rx(void)
{
    return s_zb_rx_seen ? (uint32_t)(sx_get_tick_ms() - s_zb_rx_tick) : UINT32_MAX;
}
#endif

static int zb_write(void *ctx, const uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    (void)ctx;
    /* never below the time the frame needs on the wire (12 bit times per character) */
    const uint32_t need = (((uint32_t)len * 12U * 1000U) / s_zb_uart_cfg.baudrate) + 20U;
    return sx_uart_write(&s_zb_uart, buf, (int)len, (timeout_ms > need) ? timeout_ms : need);
}

/* Slave ID 236 / 237 start the module's own point-to-point commands (0xEC / 0xED, DRF guide
 * 5.4/5.5). The COMMIT is refused by zb_validate() below; zb_status() still SAYS so
 * (LINK_STATE = ERROR / ERROR_CODE 1) for an image already stored in Flash, which the library
 * loads without asking the port. */
static bool zb_validate(void *ctx, const splc_zigbee_config_t *draft)
{
    (void)ctx;
    if (draft->enable && draft->role_slave &&
        (draft->slave_id == 236U || draft->slave_id == 237U)) {
        log_warn(TAG, "Slave ID %u refused: the module uses 0xEC/0xED as its own commands",
                 (unsigned)draft->slave_id);
        return false;
    }
    return true;
}

static bool zb_status(void *ctx, splc_zigbee_status_t *out)
{
    (void)ctx;
    splc_zigbee_config_t cfg;
    if (!splc_zigbee_get_config(&cfg, NULL) || !cfg.enable || !cfg.role_slave) {
        return false;
    }
    if (cfg.slave_id == 236U || cfg.slave_id == 237U) {
        out->link_state = PLC_ZIGBEE_LINK_ERROR;
        out->error_code = ZB_ERR_BAD_SLAVE_ID;
        out->node_count = 0U;
        out->rssi       = 0;
        return true;
    }
    return false;   /* no module status without the INS driver: shown as zeros */
}

void splc_zigbee_port_init(void)
{
    s_zb_uart_cfg.pDriver  = &SPLC_ZIGBEE_PORT_UART;
    s_zb_uart_cfg.baudrate = ZB_DEFAULT_BAUD;
    s_zb_uart_cfg.bits     = 8;
    s_zb_uart_cfg.parity   = 0;
    s_zb_uart_cfg.stopbits = 1;
    s_zb_uart.rxBuffer     = s_zb_rx_buf;
    s_zb_uart.rxBufferSize = (int)sizeof(s_zb_rx_buf);
    s_zb_uart.txBuffer     = s_zb_tx_buf;
    s_zb_uart.txBufferSize = (int)sizeof(s_zb_tx_buf);
    sx_uart_init(&s_zb_uart, &s_zb_uart_cfg);

    static const splc_zigbee_ops_t ops = {
        .ctx      = NULL,
        .roles    = SPLC_ZIGBEE_ROLE_SLAVE | SPLC_ZIGBEE_ROLE_MASTER,
        .status   = zb_status,
        .validate = zb_validate,
        .read     = zb_read,
        .write    = zb_write,
        .baudrate = ZB_DEFAULT_BAUD,
    };
    if (splc_zigbee_register(&ops)) {
        log_info(TAG, "Zigbee port registered on USART2 (%lu baud, Slave + Master)",
                 (unsigned long)ZB_DEFAULT_BAUD);
    } else {
        log_error(TAG, "Zigbee port register FAILED");
    }
}

#endif /* SPLC_ZIGBEE_ENABLE */