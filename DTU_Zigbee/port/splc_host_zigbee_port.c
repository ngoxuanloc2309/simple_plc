#include "splc_host_zigbee_port.h"
#include "splc_opt.h"

#if SPLC_ZIGBEE_ENABLE

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "splc_log.h"
#include "modbus_serial.h"      /* modbus_serial_port_t (used by plc_zigbee.h) */
#include "plc_zigbee.h"         /* splc_zigbee_register() */
#include "splc_zigbee_port.h"   /* splc_zigbee_port_ms_since_rx() (Zigbee_IO/port) */

/*
 * UART9 (DRF1609H) belongs to the host's rf_app: it reads every byte through sx_uart to parse the
 * module's ':...' text lines and sends its own commands. A Modbus RTU byte pipe on the same
 * stream would make both readers steal each other's bytes, so this port gives the library NO
 * read/write pipe. Consequence: only the Master (Coordinator) role is declared, no Modbus Slave
 * over Zigbee yet. Giving the library a pipe needs a decision about who owns UART9 (rf_app hands
 * the stream over, or rf_app forwards the non-':' frames through a ring buffer); until then this
 * port only declares the capability. NOT verified on hardware.
 */

/* No byte of the module is seen by the library: the indicator port reads "never received". */
uint32_t splc_zigbee_port_ms_since_rx(void)
{
    return UINT32_MAX;
}

void splc_host_zigbee_port_init(void)
{
    static const splc_zigbee_ops_t ops = {
        .ctx      = NULL,
        .roles    = SPLC_ZIGBEE_ROLE_MASTER,
        .status   = NULL,
        .validate = NULL,
        .read     = NULL,
        .write    = NULL,
        .baudrate = 0U,
    };
    if (splc_zigbee_register(&ops)) {
        log_info("HOST_ZIGBEE", "Zigbee capability registered (Master role only, no byte pipe)");
    } else {
        log_error("HOST_ZIGBEE", "Zigbee port register FAILED");
    }
}

#else
void splc_host_zigbee_port_init(void) {}
#endif
