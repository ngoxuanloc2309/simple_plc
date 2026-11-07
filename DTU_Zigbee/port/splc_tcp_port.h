#ifndef SPLC_TCP_PORT_H
#define SPLC_TCP_PORT_H
/*
 * splc_tcp_port.h - product port (outside the simple_plc library)
 *
 * App link over Modbus TCP, on the host's lwIP (BSD sockets, NO_SYS 0). One listening socket on
 * SPLC_TCP_PORT_NUMBER, ONE client at a time: the device is configured by one App at a time (the
 * library shares all state between transports). A new connection REPLACES the current one, so a
 * client that vanished without a FIN (the host has LWIP_TCP_KEEPALIVE 0) cannot lock the port.
 *
 * Security: Modbus TCP has no authentication. splc_tcp_port_set_filter() lets the host accept
 * only some peers; without it every peer that can reach the port is accepted.
 */
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Return true to accept the peer. ip_be = IPv4 address in network byte order. */
typedef bool (*splc_tcp_port_filter_fn)(uint32_t ip_be);
void splc_tcp_port_set_filter(splc_tcp_port_filter_fn fn);

/* Adds the TCP transport to the Modbus config server (plc_modbus_cfg_add_transport()). Call from
 * the engine task AFTER plc_engine_init(). The socket is opened lazily on the first read(), so
 * this can run before the network interface is up. Returns false if the library refused it.
 * Compiled to a log message + false when SPLC_MODBUS_MAX_TRANSPORTS < 2. */
bool splc_tcp_port_attach(void);

#ifdef __cplusplus
}
#endif
#endif /* SPLC_TCP_PORT_H */