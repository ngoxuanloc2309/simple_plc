#include "splc_mqtt_port.h"
#include "splc_opt.h"

#if SPLC_MQTT_ENABLE

#include "splc_log.h"
#include "plc_mqtt.h"       /* splc_mqtt_register() */

static splc_mqtt_port_publish_fn s_backend;
static void                     *s_backend_ctx;

void splc_mqtt_port_set_backend(splc_mqtt_port_publish_fn fn, void *ctx)
{
    s_backend_ctx = ctx;
    s_backend     = fn;
}

static int mqtt_forward(const char *topic, const char *payload, size_t len,
                        uint8_t qos, bool retain, void *ctx)
{
    (void)ctx;
    if (s_backend == NULL) {
        return -1;      /* no MQTT client yet: the library retries after SPLC_MQTT_RETRY_MS */
    }
    return s_backend(topic, payload, len, qos, retain, s_backend_ctx);
}

void splc_mqtt_port_init(void)
{
    if (!splc_mqtt_register(mqtt_forward, NULL)) {
        log_error("MQTT_PORT", "splc_mqtt_register failed");
    }
}

#else

void splc_mqtt_port_set_backend(splc_mqtt_port_publish_fn fn, void *ctx) { (void)fn; (void)ctx; }
void splc_mqtt_port_init(void) {}

#endif