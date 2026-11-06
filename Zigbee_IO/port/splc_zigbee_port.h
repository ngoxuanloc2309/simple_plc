#ifndef SPLC_ZIGBEE_PORT_H
#define SPLC_ZIGBEE_PORT_H

/*
 * splc_zigbee_port.h - product port (outside the simple_plc library)
 *
 * The Zigbee-IO board talks to its DRF1609H module over USART2 (PA2 TX / PA3 RX).
 * This port OWNS that UART: the board file (board_zigbee_io.c) only calls
 * splc_zigbee_port_init() and knows nothing else about the module.
 *
 * What the port does today:
 *   - brings up USART2 as a byte pipe (sx_uart, 512 B RX ring),
 *   - registers it with the library (splc_zigbee_register(), Slave + Master roles).
 * What it does NOT do yet (next steps, see docs/handoff.md decision 98):
 *   - configure the module with the INS commands (Zigbee_IO/service/zigbee),
 *   - share the pipe between Modbus bytes and INS commands.
 *
 * Compiled to an empty unit when SPLC_ZIGBEE_ENABLE = 0, so a build without Zigbee
 * is not affected by this folder.
 *
 * Prototype is also declared in board_zigbee_io.h (identical on purpose: the board
 * library must not need this folder on its include path).
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Call once from board_hw_init(), after logger_init(). Safe to call only when
 * SPLC_ZIGBEE_ENABLE = 1 (the symbol does not exist otherwise). */
void splc_zigbee_port_init(void);

#ifdef __cplusplus
}
#endif

#endif /* SPLC_ZIGBEE_PORT_H */