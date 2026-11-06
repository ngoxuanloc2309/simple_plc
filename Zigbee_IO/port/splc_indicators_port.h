#ifndef SPLC_INDICATORS_PORT_H
#define SPLC_INDICATORS_PORT_H

/*
 * splc_indicators_port.h - product port (outside the simple_plc library)
 *
 * Wires the library's connection indicator (services/plc_indicator, handoff decision 99,
 * docs/plc_indicator_design.md) to the three LEDs of the Zigbee-IO board:
 *
 *     POWER   PC15  (CubeMX label LED2)
 *     STATUS  PC14  (CubeMX label LED1)
 *     LINK    PC13  (CubeMX label LED0)
 *
 * The pins are push-pull outputs, low, set up by CubeMX (MX_GPIO_Init); this port only writes
 * them. Default polarity is ACTIVE HIGH (pin high = LED lit); define SPLC_IND_LED_ACTIVE_LOW=1
 * (compiler flag or splcopts.h) if the LEDs are wired to 3V3.
 *
 * Protocols registered, in priority order:
 *     1. Zigbee  (only when SPLC_ZIGBEE_ENABLE = 1)
 *
 * Compiled to an empty unit when SPLC_INDICATOR_ENABLE = 0, so a build without the indicator
 * is not affected by this folder. Same pattern as splc_zigbee_port.h: the prototype is also
 * declared in board_zigbee_io.h (identical on purpose, the board library must not need this
 * folder on its include path).
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Call once from board_hw_init(), after logger_init() and splc_zigbee_port_init(), and before
 * the first engine scan (the library closes its table at the first scan). Lights POWER. Safe to
 * call only when SPLC_INDICATOR_ENABLE = 1 (the symbol does not exist otherwise). */
void splc_indicators_port_init(void);

#ifdef __cplusplus
}
#endif

#endif /* SPLC_INDICATORS_PORT_H */