#ifndef SPLC_HOST_RS485_PORT_H
#define SPLC_HOST_RS485_PORT_H
/*
 * splc_host_rs485_port.h - product port (outside the simple_plc library)
 *
 * SimplePLC owns no UART here: the HOST firmware installs the three RS485 UARTs with sx_uart
 * (board/dev/uart/sx_uart.h). This port shapes them into the modbus_serial_port_t the library
 * wants and registers them with splc_dtu_set_rs485_port(). The host does NOT drive the DE pins
 * (they are plain GPIO outputs set low by MX_GPIO_Init), so this port raises DE around each
 * frame itself (RE and DE are tied together on the board: no echo).
 *
 * Mapping, App port number -> UART (same order as the host's settings and sx_uart ports, see
 * board_dtu_zigbee.h): 1 = UART7, 2 = USART3, 3 = USART10.
 *
 * HOST REQUIREMENT: the three UARTs must be installed in sx_uart INTERRUPT or DMA mode with an RX
 * buffer (rx_buffer_size > 0). The host's settings_install_uarts() currently uses POLLING with no
 * RX buffer: there sx_uart_get_rx_bytes_available() returns -1 and this port can read nothing.
 *
 * Line speed told to the library = the UART's actual baud rate at init time.
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
 * writes the line settings (MBM-003). `ctx` passed to it is NULL. Not provided here: the host
 * owns the settings (settings_install_uarts()). */
void splc_host_rs485_port_set_configure(plc_mbm_configure_fn fn);
#endif

/* Register the ports with the board (splc_dtu_set_rs485_port). Call before plc_engine_init(),
 * from the engine task. */
void splc_host_rs485_port_init(void);

#ifdef __cplusplus
}
#endif
#endif /* SPLC_HOST_RS485_PORT_H */
