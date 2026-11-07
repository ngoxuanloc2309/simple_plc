#ifndef SPLC_MQTT_PORT_H
#define SPLC_MQTT_PORT_H
/*
 * splc_mqtt_port.h - product port (outside the simple_plc library)
 *
 * The library renders MQTT topics/payloads and hands them to ONE publish callback it never
 * implements (splc_mqtt_register(), decision 16). This port registers that callback and forwards
 * to a BACKEND the host plugs in with splc_mqtt_port_set_backend() (the host's MQTT client).
 * Until a backend is set every publish returns -1: the library retries later and loses nothing.
 *
 * The connection status goes the other way: the host calls splc_mqtt_set_status() (plc_mqtt.h).
 * Compiled empty unless SPLC_MQTT_ENABLE = 1.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Same contract as splc_mqtt_publish_fn: runs in the engine task, must not block,
 * payload is NUL-terminated and reused after the call. Return 0 = accepted, < 0 = busy/failed. */
typedef int (*splc_mqtt_port_publish_fn)(const char *topic, const char *payload, size_t len,
                                         uint8_t qos, bool retain, void *ctx);

/* Host side: plug the MQTT client in (NULL unplugs). Safe to call before or after splc_port_init(). */
void splc_mqtt_port_set_backend(splc_mqtt_port_publish_fn fn, void *ctx);

/* Register the forwarder with the library. Call before plc_engine_init(), from the engine task. */
void splc_mqtt_port_init(void);

#ifdef __cplusplus
}
#endif
#endif /* SPLC_MQTT_PORT_H */