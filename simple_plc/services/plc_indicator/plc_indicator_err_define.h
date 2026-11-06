#ifndef PLC_INDICATOR_ERR_DEFINE_H
#define PLC_INDICATOR_ERR_DEFINE_H

/*
 * plc_indicator_err_define.h - types shared by the connection indicator and the product's
 * port file (splc_indicators_port.{h,c}). See docs/plc_indicator_design.md sections 2, 4, 9.
 * Nothing here depends on hardware.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The three LEDs. The library decides what each shows; the port maps them to pins. */
typedef enum {
    SPLC_LED_POWER  = 0,   /* lit from init, never changes            */
    SPLC_LED_STATUS = 1,   /* state of the protocol shown on LINK     */
    SPLC_LED_LINK   = 2,   /* which protocol is being reported        */
    SPLC_LED_COUNT  = 3
} SPLC_LED_ID;

/* The four states every protocol is reduced to by its get_state() (design section 2).
 * Mapping from plc_zigbee: OFF -> DISABLED, JOINING -> CONNECTING, UP -> CONNECTED,
 * ERROR -> FAILED. */
typedef enum {
    SPLC_LINK_STATE_DISABLED   = 0,   /* not enabled / not configured: ignored   */
    SPLC_LINK_STATE_CONNECTING = 1,   /* joining, associating, DHCP, registering */
    SPLC_LINK_STATE_CONNECTED  = 2,   /* usable                                  */
    SPLC_LINK_STATE_FAILED     = 3    /* enabled but not usable                  */
} SPLC_LINK_STATE;

/* Diagnostic detail a port may keep next to its state. It does NOT change what the LEDs
 * show in this version (design section 2). */
typedef enum {
    SPLC_LINK_ERR_NONE    = 0,
    SPLC_LINK_ERR_NO_PEER = 1,   /* nothing answers (no coordinator, no AP, no cable) */
    SPLC_LINK_ERR_AUTH    = 2,   /* credentials / key rejected                        */
    SPLC_LINK_ERR_CONFIG  = 3,   /* configuration refused by the module               */
    SPLC_LINK_ERR_HW      = 4    /* module does not answer                            */
} SPLC_LINK_ERR;

/* LED LINK pattern of one protocol, chosen by the product. One cycle =
 * count * (on_ms + off_ms) + pause_ms.  count = 1, pause_ms = 0: plain periodic blink;
 * count = N, long pause_ms: "N flashes, pause". on_ms and count must be non-zero, and
 * off_ms non-zero when count > 1 (register() rejects the rest). */
typedef struct {
    uint16_t on_ms;
    uint16_t off_ms;
    uint8_t  count;
    uint16_t pause_ms;
} splc_led_pattern_t;

#ifdef __cplusplus
}
#endif

#endif /* PLC_INDICATOR_ERR_DEFINE_H */