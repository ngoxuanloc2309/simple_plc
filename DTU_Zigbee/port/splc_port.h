#ifndef SPLC_PORT_H
#define SPLC_PORT_H
/*
 * splc_port.h - DTU Zigbee product port: the ONE entry point the host firmware calls.
 *
 *   splc_port_start()  - creates the SimplePLC engine task (FreeRTOS). Call it ONCE from the host,
 *                        after the host has started TinyUSB (usb_netif_init() in app_init()) and
 *                        after the scheduler's heap is usable. That is the only line the host adds.
 *
 * What the task does, in this order (the order is the contract of the library):
 *   1. splc_port_init()           register what the HOST owns as byte ports (RS485 x3, Zigbee, App
 *                                 link over USB-CDC), plus the RTC hooks and the MQTT forwarder
 *                                 -- everything the library wants BEFORE plc_engine_init()
 *   2. plc_engine_init()          board_hw_init() (forwards RS485 ports, indicators), config, Flash,
 *                                 then board_get_modbus_transport() reads the registered link
 *   3. splc_tcp_port_attach()     the second App transport (Modbus TCP), after plc_engine_init()
 *   4. plc_engine_poll() forever  non-blocking, so the loop sleeps 1 tick between calls
 *
 * Everything runs in this one task: splc_link_input(), the link callbacks and the TCP transport
 * all stay in the engine context the library requires.
 */
#ifdef __cplusplus
extern "C" {
#endif

/* Step 1 alone, for a host that runs the engine in a task of its own. */
void splc_port_init(void);

/* Create the engine task. Returns 0 on success, -1 if the task could not be created. */
int splc_port_start(void);

#ifdef __cplusplus
}
#endif
#endif /* SPLC_PORT_H */