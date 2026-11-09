#include "splc_host_zigbee_port.h"
#include "splc_opt.h"

#if SPLC_ZIGBEE_ENABLE

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "splc_log.h"
#include "modbus_serial.h"      /* modbus_serial_port_t (used by plc_zigbee.h) */
#include "plc_zigbee.h"         /* splc_zigbee_register(), splc_zigbee_get_config() */
#include "sx_time.h"            /* sx_get_tick_ms() */
#include "splc_zigbee_port.h"   /* splc_zigbee_port_ms_since_rx() (Zigbee_IO/port) */
#include "board.h"              /* HOST: uart_*(), RF_PORT */

#ifndef SPLC_HOST_ZIGBEE_BAUD
#define SPLC_HOST_ZIGBEE_BAUD 115200U
#endif

static const char *TAG = "HOST_ZIGBEE";

static bool     s_rx_seen;
static uint32_t s_rx_tick;
/* same counters as Zigbee_IO/port/splc_zigbee_port.c: frames written, non-empty reads (a traffic
 * indicator, not a count of Zigbee packets); the host's UART write returns no error here. */
static uint16_t s_tx_pkts, s_rx_pkts;

uint32_t splc_zigbee_port_ms_since_rx(void)
{
    return s_rx_seen ? (uint32_t)(sx_get_tick_ms() - s_rx_tick) : UINT32_MAX;
}

/* Non-blocking: see splc_host_rs485_port.c for why the queue length is asked first. */
static int zb_read(void *ctx, uint8_t *buf, uint16_t max_len)
{
    (void)ctx;
    size_t n = 0;
    if (uart_get_buffered_data_len(RF_PORT, &n) != 0 || n == 0U) {
        return 0;
    }
    if (n > max_len) {
        n = max_len;
    }
    const int r = uart_read_bytes(RF_PORT, buf, (uint32_t)n, 2U);
    if (r <= 0) {
        return 0;
    }
    s_rx_seen = true;
    s_rx_tick = sx_get_tick_ms();
    s_rx_pkts++;
    return r;
}

/* Blocking send (host: HAL_UART_Transmit under the port mutex). The module has no DE pin and no
 * echo, so nothing to filter. */
static int zb_write(void *ctx, const uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    (void)ctx;
    (void)timeout_ms;
    uart_write_bytes(RF_PORT, buf, len);
    s_tx_pkts++;
    return 0;
}

/* Slave ID 236 / 237 start the module's own point-to-point commands (DRF guide 5.4/5.5). The
 * COMMIT is refused by zb_validate() below; zb_status() still SAYS so (ERROR_CODE 3 = RESTRICTED_ID, same as Zigbee_IO) for an
 * image already stored in Flash, which the library loads without asking the port. */
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
    out->tx_packets = s_tx_pkts;
    out->rx_packets = s_rx_pkts;
    splc_zigbee_config_t cfg;
    if (splc_zigbee_get_config(&cfg, NULL) && cfg.enable && cfg.role_slave &&
        (cfg.slave_id == 236U || cfg.slave_id == 237U)) {
        out->link_state = PLC_ZIGBEE_LINK_ERROR;
        out->error_code = PLC_ZIGBEE_ERR_RESTRICTED_ID;
    }
    return true;
}

void splc_host_zigbee_port_init(void)
{
    static const splc_zigbee_ops_t ops = {
        .ctx      = NULL,
        .roles    = SPLC_ZIGBEE_ROLE_SLAVE | SPLC_ZIGBEE_ROLE_MASTER,
        .status   = zb_status,
        .validate = zb_validate,
        .read     = zb_read,
        .write    = zb_write,
        .baudrate = SPLC_HOST_ZIGBEE_BAUD,
    };
    if (splc_zigbee_register(&ops)) {
        log_info(TAG, "Zigbee port registered on the host's RF port (%lu baud, Slave + Master)",
                 (unsigned long)SPLC_HOST_ZIGBEE_BAUD);
    } else {
        log_error(TAG, "Zigbee port register FAILED");
    }
}

#else
void splc_host_zigbee_port_init(void) {}
#endif