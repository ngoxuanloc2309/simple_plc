#include "splc_indicators_port.h"
#include "splc_opt.h"

#if SPLC_INDICATOR_ENABLE

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "main.h"             /* CubeMX-generated: LED0/LED1/LED2 pin labels, HAL GPIO */
#include "splc_log.h"
#include "plc_indicator.h"    /* library: splc_indicator_init(), splc_indicator_register() */

#if SPLC_ZIGBEE_ENABLE
#include "modbus_serial.h"    /* modbus_serial_port_t (used by plc_zigbee.h) */
#include "plc_zigbee.h"       /* splc_zigbee_get_config() */
#include "splc_zigbee_port.h" /* splc_zigbee_port_ms_since_rx() */
#endif

/* ---- LEDs ------------------------------------------------------------------------------- */

/* Polarity: 0 = pin HIGH lights the LED (assumption, not confirmed on the schematic),
 * 1 = pin LOW lights it. */
#ifndef SPLC_IND_LED_ACTIVE_LOW
#define SPLC_IND_LED_ACTIVE_LOW 0
#endif

typedef struct {
    GPIO_TypeDef *port;
    uint16_t      pin;
} led_pin_t;

/* Indexed by SPLC_LED_ID. CubeMX labels: LED2 = PC15, LED1 = PC14, LED0 = PC13. */
static const led_pin_t s_led[SPLC_LED_COUNT] = {
    [SPLC_LED_POWER]  = { LED2_GPIO_Port, LED2_Pin },
    [SPLC_LED_STATUS] = { LED1_GPIO_Port, LED1_Pin },
    [SPLC_LED_LINK]   = { LED0_GPIO_Port, LED0_Pin },
};

/* GPIO write only: no waiting, no logging (library contract, plc_indicator.h). */
static void led_set(SPLC_LED_ID led, bool on, void *ctx)
{
    (void)ctx;
    if ((unsigned)led >= (unsigned)SPLC_LED_COUNT) {
        return;
    }
    HAL_GPIO_WritePin(s_led[led].port, s_led[led].pin,
                      (on != (SPLC_IND_LED_ACTIVE_LOW != 0)) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/* ---- Zigbee ----------------------------------------------------------------------------- */

#if SPLC_ZIGBEE_ENABLE

/* LED LINK pattern of Zigbee: one 100 ms flash every 1.5 s. */
#define ZB_PATTERN_ON_MS    100U
#define ZB_PATTERN_OFF_MS   1400U

/* Zigbee state shown on the LEDs.
 *
 * LIMIT: there is no module status yet (the INS-command driver, Zigbee_IO/service/zigbee, is
 * not wired in), so the module's real network state is NOT known. The state is derived from
 * what the port does know:
 *   - block 0x1700 not enabled ............................ DISABLED (ignored by the LEDs)
 *   - enabled, Slave role, Slave ID 236 / 237 (the DRF module
 *     eats 0xEC / 0xED as its own commands, handoff 97a) ..... FAILED
 *   - enabled, no byte received from the module yet ........ CONNECTING
 *   - enabled, a byte received from the module ............. CONNECTED
 * "A byte received" means the coordinator reached this node through the module. It latches by
 * default (SPLC_IND_ZB_RX_LOST_MS = 0) because a healthy but idle network sends nothing; set a
 * timeout (ms) to fall back to CONNECTING when the coordinator has been silent that long.
 * Replace this function with the module's real state once the INS driver reports it. */
#ifndef SPLC_IND_ZB_RX_LOST_MS
#define SPLC_IND_ZB_RX_LOST_MS 0U
#endif

static SPLC_LINK_STATE zigbee_state(void *ctx)
{
    (void)ctx;
    splc_zigbee_config_t cfg;
    if (!splc_zigbee_get_config(&cfg, NULL) || !cfg.enable) {
        return SPLC_LINK_STATE_DISABLED;
    }
    if (cfg.role_slave && ((cfg.slave_id == 236U) || (cfg.slave_id == 237U))) {
        return SPLC_LINK_STATE_FAILED;
    }
    const uint32_t quiet = splc_zigbee_port_ms_since_rx();
    if (quiet == UINT32_MAX) {
        return SPLC_LINK_STATE_CONNECTING;
    }
    if ((SPLC_IND_ZB_RX_LOST_MS != 0U) && (quiet > SPLC_IND_ZB_RX_LOST_MS)) {
        return SPLC_LINK_STATE_CONNECTING;
    }
    return SPLC_LINK_STATE_CONNECTED;
}

#endif /* SPLC_ZIGBEE_ENABLE */

/* ---- init ------------------------------------------------------------------------------- */

static const char *TAG = "IND_PORT";

void splc_indicators_port_init(void)
{
    static const splc_ind_ops_t ops = { .set = led_set, .ctx = NULL };

    if (!splc_indicator_init(&ops)) {
        log_error(TAG, "indicator init FAILED");
        return;
    }

#if SPLC_ZIGBEE_ENABLE
    static const splc_ind_proto_t zigbee = {
        .name       = "zigbee",
        .pattern    = { .on_ms = ZB_PATTERN_ON_MS, .off_ms = ZB_PATTERN_OFF_MS,
                        .count = 1U, .pause_ms = 0U },
        .get_state  = zigbee_state,
        .ctx        = NULL,
        .depends_on = 0U,
    };
    if (splc_indicator_register(&zigbee) < 0) {
        log_error(TAG, "register zigbee FAILED");
    }
#endif

    log_info(TAG, "indicator ready (POWER PC15, STATUS PC14, LINK PC13, active %s)",
             SPLC_IND_LED_ACTIVE_LOW ? "low" : "high");
}

#endif /* SPLC_INDICATOR_ENABLE */