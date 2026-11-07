#ifndef SPLC_HOST_ZIGBEE_PORT_H
#define SPLC_HOST_ZIGBEE_PORT_H
/*
 * splc_host_zigbee_port.h - product port (outside the simple_plc library)
 *
 * The Zigbee module (DRF1609H, transparent mode) hangs on UART9, which the HOST owns
 * (SynaptiX/board/board.c, RF_PORT). This port turns the host's uart_*() on RF_PORT into the byte
 * pipe of splc_zigbee_register() (Slave + Master roles). It replaces Zigbee_IO's
 * splc_zigbee_port.c for this product: that file brings the UART up itself (sx_uart), which here
 * would be a second owner of UART9.
 *
 * Line speed told to the library: SPLC_HOST_ZIGBEE_BAUD (default 115200) must equal the real
 * speed of UART9 and of the module.
 *
 * Also defines splc_zigbee_port_ms_since_rx() for the indicator port (Zigbee_IO/port).
 * Compiled empty unless SPLC_ZIGBEE_ENABLE = 1.
 */
#ifdef __cplusplus
extern "C" {
#endif

/* Register with the library. Call before plc_engine_init(), from the engine task. The host must
 * have called uart_init() for RF_PORT first. */
void splc_host_zigbee_port_init(void);

#ifdef __cplusplus
}
#endif
#endif /* SPLC_HOST_ZIGBEE_PORT_H */