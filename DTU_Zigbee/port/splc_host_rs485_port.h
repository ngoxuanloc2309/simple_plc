#ifndef SPLC_HOST_RS485_PORT_H
#define SPLC_HOST_RS485_PORT_H
/*
 * splc_host_rs485_port.h - product port (outside the simple_plc library)
 *
 * SimplePLC owns no hardware here: the HOST firmware owns the three RS485 UARTs, their DE pins
 * and the receive queues (SynaptiX/board/board.c: uart_write_bytes() drives DE, the RX interrupt
 * fills a queue read by uart_read_bytes()). This port only shapes those host functions into the
 * modbus_serial_port_t the library wants and registers them with splc_dtu_set_rs485_port().
 *
 * Mapping, App port number -> host port (board.h numbering; override with the macros):
 *   App port 1 -> SPLC_HOST_RS485_PORT_1 (default RS485_0_PORT = UART7)
 *   App port 2 -> SPLC_HOST_RS485_PORT_2 (default RS485_1_PORT = USART3)
 *   App port 3 -> SPLC_HOST_RS485_PORT_3 (default RS485_2_PORT = USART10)
 * NOTE: the earlier board file numbered them USART3 / UART7 / USART10. Check which one is printed
 * "RS485-1" on the enclosure.
 *
 * Line speed: the host sets the UART. SPLC_HOST_RS485_BAUD is only what the library is TOLD
 * (default 9600-8-N-1); it must equal the real speed. Unless the host gives a configure hook the
 * App's line settings are ignored.
 *
 * Compiled empty unless SPLC_MBM_ENABLE = 1.
 */
#include "splc_opt.h"
#if SPLC_MBM_ENABLE
#include "plc_mbm.h"    /* plc_mbm_configure_fn */
#endif

#ifdef __cplusplus
extern "C" {
#endif

#if SPLC_MBM_ENABLE
/* Optional, call BEFORE splc_host_rs485_port_init(): the host's way to change a UART when the App
 * writes the line settings (MBM-003). `ctx` passed to it is the HOST port number as
 * (void *)(intptr_t). Not provided here on purpose: SynaptiX's app_setup_serial() sets
 * Instance = USART1 for every port, so it cannot be reused as is. */
void splc_host_rs485_port_set_configure(plc_mbm_configure_fn fn);
#endif

/* Register the ports with the board (splc_dtu_set_rs485_port). Call before plc_engine_init(),
 * from the engine task. The host must have called uart_init() for those ports first. */
void splc_host_rs485_port_init(void);

#ifdef __cplusplus
}
#endif
#endif /* SPLC_HOST_RS485_PORT_H */