#ifndef SPLC_PORT_H
#define SPLC_PORT_H

/*
 * splc_port.h - glue between the GatewayV2 host firmware and SimplePLC.
 *
 * SimplePLC is embedded the way lwIP is: the host supplies a small port file
 * (this one) and an options file (splcopts.h next to it). Nothing inside
 * SimplePLC is edited, and none of the host's drivers are edited either.
 *
 * What this port plugs in:
 *   - RS485 master port to the I/O boards : splc_gateway_set_rs485_port()
 *       built on the host driver Drivers/uart/rs485_driver.c (owner
 *       RS485_OWNER_MODBUS_GATEWAY, so modbus_master / serial_tcp_server
 *       cannot take the same port: their rs485_port_acquire() fails).
 *   - App <-> device byte pipe            : splc_link_register()
 *       built on TinyUSB CDC instance 0 (the CDC-ACM of the composite
 *       descriptor). The CDC exists only after usb_netif_task ran tusb_init(),
 *       i.e. only with GW_USB_NETIF_ENABLE=1 and after WiFi got an IP.
 *   - One FreeRTOS task that calls plc_engine_init() and then plc_engine_poll().
 *       SimplePLC is single-threaded: no other task may touch its tags/rules.
 *
 * Call splc_host_start() ONCE from gateway.c, AFTER rs485_port_init() and
 * modbus_master_init() (the RS485 port only becomes RUNNING in
 * rs485_port_reconfigure(); this port also retries by itself every second).
 */

#include <stdbool.h>
#include "splc_opt.h"        /* pulls in splcopts.h of this product */

/* ---- Options of THIS port (override in splcopts.h) ---------------------- */

/* RS485 port used for the I/O boards: 0 = USART3, 1 = USART6, 2 = USART10
 * (index of rs485_driver.h). Must not be configured for any host service. */
#ifndef SPLC_HOST_RS485_PORT
#define SPLC_HOST_RS485_PORT      1
#endif

/* Line settings of the I/O boards' RS485 bus. parity: 0 none, 1 odd, 2 even
 * (rs485_driver.c). Data bits 8, stop bits 1. */
#ifndef SPLC_HOST_RS485_BAUD
#define SPLC_HOST_RS485_BAUD      115200U
#endif
#ifndef SPLC_HOST_RS485_PARITY
#define SPLC_HOST_RS485_PARITY    0
#endif

/* Engine task. Stack is in WORDS (4 bytes). Priority must stay above
 * usb_netif_task (5) so the scan keeps its 10 ms period, and below the
 * application band (10..12) so a Flash erase or a log burst cannot starve it. */
#ifndef SPLC_HOST_TASK_STACK_WORDS
#define SPLC_HOST_TASK_STACK_WORDS 1536U
#endif
#ifndef SPLC_HOST_TASK_PRIO
#define SPLC_HOST_TASK_PRIO        6U
#endif

#if (SPLC_HOST_RS485_PORT < 0) || (SPLC_HOST_RS485_PORT > 2)
#error "SPLC_HOST_RS485_PORT must be 0 (USART3), 1 (USART6) or 2 (USART10)"
#endif
#if (SPLC_HOST_RS485_PARITY < 0) || (SPLC_HOST_RS485_PARITY > 2)
#error "SPLC_HOST_RS485_PARITY must be 0, 1 or 2"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Register the RS485 port and the App link, then create the engine task.
 * Returns false (and logs why) when called twice, when registration fails or
 * when the task cannot be created. */
bool splc_host_start(void);

#ifdef __cplusplus
}
#endif

#endif /* SPLC_PORT_H */