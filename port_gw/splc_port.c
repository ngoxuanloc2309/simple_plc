/*
 * splc_port.c - GatewayV2 host port of SimplePLC (see splc_port.h).
 */
#include "splc_port.h"

#include <stdint.h>
#include <stdbool.h>

#include "FreeRTOS.h"
#include "task.h"
#include "tusb.h"
#include "rs485_driver.h"

#include "plc_engine.h"
#include "modbus_link.h"
#include "modbus_serial.h"
#include "board_ethernet_wifi_gateway.h"
#include "splc_log.h"

static const char *TAG = "SPLC_PORT";

/* The CDC-ACM is the only CDC class instance of the host's descriptor
 * (ports/usb_port/Target/usb_descriptors.c); the network function is RNDIS /
 * ECM / NCM, not a CDC class instance. */
#define SPLC_CDC_ITF            0

/* Set by usb_netif_task after tusb_init() succeeded (Core/Src/usb.c).
 * Until then no tud_*() call is allowed. */
extern volatile uint8_t g_usb_tusb_initialized;

#define SPLC_PORT_CHECK_MS      1000U

/* ------------------------------------------------------------------------ */
/* RS485: modbus_serial_port_t on top of the host driver                     */
/* ------------------------------------------------------------------------ */

/* Non-blocking read. NOTE: rs485_recv_polling() counts every empty read in
 * counters.timeouts of this port (host driver behaviour, left untouched), so
 * that counter is meaningless for the SimplePLC port. */
static int rs485_read_cb(void *ctx, uint8_t *buf, uint16_t max_len)
{
    (void)ctx;
    int32_t n = rs485_recv_polling((uint8_t)SPLC_HOST_RS485_PORT, buf, max_len, 0U);
    return (n > 0) ? (int)n : 0;
}

/* rs485_send() drives DE itself and returns after the last bit left the UART
 * (it waits for TC), which is exactly the contract of modbus_serial_port_t.
 * Its own timeout (1 s) is fixed inside the driver, so timeout_ms is unused. */
static int rs485_write_cb(void *ctx, const uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    (void)ctx;
    (void)timeout_ms;
    int32_t n = rs485_send((uint8_t)SPLC_HOST_RS485_PORT, buf, len);
    return (n == (int32_t)len) ? 0 : -1;
}

/*
 * Keep the RS485 port ours and configured. Runs in the engine task every
 * SPLC_PORT_CHECK_MS. The host can change the port under us (web UI, config
 * apply) or may not have initialised it yet, so this is a loop, not a one-off:
 *   - take ownership as RS485_OWNER_MODBUS_GATEWAY (fails while another
 *     service owns the port: then we only log, once per change),
 *   - (re)apply baud / parity when the port is not RUNNING or differs.
 * While this is not satisfied rs485_send()/recv return -1 and the virtual I/O
 * nodes simply stay offline; they recover on their own afterwards.
 */
static void splc_check_port(void)
{
    static int8_t s_last_problem = -1;   /* -1 none yet, 0 ok, 1 owner, 2 init */
    const uint8_t port = (uint8_t)SPLC_HOST_RS485_PORT;
    rs485_port_status_t st;
    int8_t problem = 0;

    if (rs485_port_get_status(port, &st) != 0) {
        problem = 2;
    } else {
        if (st.owner == RS485_OWNER_NONE) {
            if (rs485_port_acquire(port, RS485_OWNER_MODBUS_GATEWAY) != 0) {
                problem = 2;             /* driver not initialised yet */
            } else {
                st.owner = RS485_OWNER_MODBUS_GATEWAY;
            }
        }
        if (problem == 0 && st.owner != RS485_OWNER_MODBUS_GATEWAY) {
            problem = 1;
        }
        if (problem == 0) {
            if (st.state != RS485_STATE_RUNNING ||
                st.config.baudrate != SPLC_HOST_RS485_BAUD ||
                st.config.parity   != SPLC_HOST_RS485_PARITY ||
                st.config.data_bits != 8U || st.config.stop_bits != 1U) {
                rs485_runtime_config_t cfg = {
                    .baudrate  = SPLC_HOST_RS485_BAUD,
                    .data_bits = 8U,
                    .parity    = (uint8_t)SPLC_HOST_RS485_PARITY,
                    .stop_bits = 1U,
                };
                if (rs485_port_reconfigure(port, &cfg) != 0) {
                    problem = 2;
                }
            }
        }
    }

    if (problem != s_last_problem) {
        if (problem == 1) {
            log_error(TAG, "RS485 port %d is owned by another host service; "
                           "virtual I/O is NOT served", SPLC_HOST_RS485_PORT);
        } else if (problem == 2) {
            log_warn(TAG, "RS485 port %d not ready (driver not initialised "
                          "or reconfigure failed), retrying", SPLC_HOST_RS485_PORT);
        } else if (s_last_problem > 0) {
            log_info(TAG, "RS485 port %d ready", SPLC_HOST_RS485_PORT);
        }
        s_last_problem = problem;
    }
}

/* ------------------------------------------------------------------------ */
/* App link: modbus_link ops on top of TinyUSB CDC instance 0                */
/* ------------------------------------------------------------------------ */

/* Everything below runs in the engine task (modbus_link contract). TinyUSB's
 * CDC FIFOs are protected by its OS mutex (CFG_TUSB_OS = FreeRTOS), so the
 * engine task may read/write them while usb_netif_task runs tud_task_ext();
 * a reply can therefore leave up to one scan (10 ms) late. */

static bool cdc_usable(void)
{
    return g_usb_tusb_initialized != 0U && tud_cdc_n_connected(SPLC_CDC_ITF);
}

/* Called once per scan and while read()/write() wait: move received bytes into
 * the core's ring. splc_link_input() is only ever called from here. */
static void cdc_process(void *ctx)
{
    (void)ctx;
    if (!cdc_usable()) {
        return;
    }
    uint8_t tmp[64];
    while (tud_cdc_n_available(SPLC_CDC_ITF) > 0U) {
        uint32_t n = tud_cdc_n_read(SPLC_CDC_ITF, tmp, sizeof(tmp));
        if (n == 0U) {
            break;
        }
        if (splc_link_input(tmp, (uint16_t)n) < n) {
            break;   /* ring full: the core counted the dropped bytes */
        }
    }
}

static int32_t cdc_output(const uint8_t *buf, uint16_t len, void *ctx)
{
    (void)ctx;
    if (!cdc_usable()) {
        return -1;   /* hard error: no USB, or the App closed the port (DTR low) */
    }
    uint32_t n = tud_cdc_n_write(SPLC_CDC_ITF, buf, len);
    (void)tud_cdc_n_write_flush(SPLC_CDC_ITF);
    return (int32_t)n;   /* 0 = FIFO full, the core retries */
}

/* ------------------------------------------------------------------------ */
/* Engine task                                                               */
/* ------------------------------------------------------------------------ */

static void splc_task(void *arg)
{
    (void)arg;
    TickType_t last_check = xTaskGetTickCount() - pdMS_TO_TICKS(SPLC_PORT_CHECK_MS);

    plc_engine_init();   /* board_hw_init() builds the virtual I/O from the port */

    for (;;) {
        if ((TickType_t)(xTaskGetTickCount() - last_check) >= pdMS_TO_TICKS(SPLC_PORT_CHECK_MS)) {
            last_check = xTaskGetTickCount();
            splc_check_port();
        }
        plc_engine_poll();
        vTaskDelay(pdMS_TO_TICKS(1U));
    }
}

bool splc_host_start(void)
{
    static bool started;
    if (started) {
        log_error(TAG, "splc_host_start() called twice");
        return false;
    }

    const modbus_serial_port_t port = {
        .read     = rs485_read_cb,
        .write    = rs485_write_cb,
        .set_tx   = NULL,            /* rs485_send() drives DE itself */
        .ctx      = NULL,
        .baudrate = SPLC_HOST_RS485_BAUD,
    };
    if (!splc_gateway_set_rs485_port(&port)) {
        log_error(TAG, "RS485 port registration failed");
        return false;
    }

    const splc_link_ops_t ops = {
        .ctx     = NULL,
        .output  = cdc_output,
        .process = cdc_process,
        .kind    = MODBUS_TRANSPORT_KIND_RTU,
        .unit_id = (uint8_t)SPLC_MODBUS_UNIT_ID,
    };
    if (splc_link_register(&ops) != SPLC_LINK_OK) {
        log_error(TAG, "App link registration failed");
        return false;
    }

    if (xTaskCreate(splc_task, "SPLC", (configSTACK_DEPTH_TYPE)SPLC_HOST_TASK_STACK_WORDS,
                    NULL, SPLC_HOST_TASK_PRIO, NULL) != pdPASS) {
        log_error(TAG, "engine task creation failed");
        splc_link_unregister();
        return false;
    }

    started = true;
    return true;
}