#ifndef PLC_WIFI_H
#define PLC_WIFI_H

/*
 * plc_wifi.h - Layer 3 (PLC Application Services)
 *
 * Wi-Fi block 0x1300..0x13FF of Wire V2.0 (ICD SPLC-SPEC-WIFI-004, 256 registers):
 *
 *   0x1300..0x133F  config + live status   64 regs  R/W (0x1339..0x133F are read-only)
 *   0x1340..0x134F  command + scan state   16 regs  R/W (only 0x1340 is writable)
 *   0x1350..0x13FF  scan results 8 x 22    176 regs R
 *
 * Same philosophy as MQTT (handoff decision 31): the library owns NO radio and NO
 * Flash for Wi-Fi. It keeps the configuration the App wrote, turns the App's commands
 * into calls of a small set of NON-BLOCKING host functions (splc_wifi_ops_t), and
 * reads the live state back from the host when the App asks for it. The host runs the
 * slow part (scan / WPA handshake take 1.5..3 s) in its own task, so a Modbus request
 * is always answered at once (ICD invariant 1, CHKL-WIFI-03/07).
 *
 * Unlike the draft/COMMIT blocks this is a COMMAND block (decision 50):
 *   - writing the configuration only updates RAM: it never connects and never saves;
 *   - read-only fields in a written range (live status) are ignored, because the App
 *     writes the whole 64-register struct with one FC16;
 *   - the command register 0x1340 reads back 0 (IDLE) at once;
 *   - the password reads back as written (user decision 23/10/2026, as in the ICD).
 *     It is also plain text in the host's own Flash record (host `save`): say so when
 *     handing the product over.
 *
 * Only declared (0x0A08 bit 0x0200) once the host registered its functions. Compiled to
 * nothing when SPLC_WIFI_ENABLE = 0.
 *
 * Context rule: every function here runs in the engine task (like all of Layer 3), and
 * so do the callbacks of splc_wifi_ops_t. The host's callbacks must return at once and
 * be safe against the host's own Wi-Fi task (they usually read/write a mutex-protected
 * state that task maintains).
 */

#include <stdint.h>
#include <stdbool.h>
#include "splc_opt.h"

#define PLC_WIFI_BASE            0x1300U
#define PLC_WIFI_REGS            256U
#define PLC_WIFI_MAX_APS         8U

/* Declared in HARDWARE_CAPABILITIES (0x0A08) once ops are registered. */
#define PLC_WIFI_CAP_WIFI        0x00000200UL

/* control_flags (0x1300) */
#define PLC_WIFI_CTRL_ENABLE          0x0001U
#define PLC_WIFI_CTRL_MODE_MASK       0x0006U     /* 0 STA, 1 SoftAP, 2 STA+AP */
#define PLC_WIFI_CTRL_MODE_SHIFT      1U
#define PLC_WIFI_CTRL_DHCP            0x0008U
#define PLC_WIFI_CTRL_AUTO_RECONNECT  0x0010U

/* conn_status (0x1339) */
#define PLC_WIFI_CONN_DISCONNECTED    0U
#define PLC_WIFI_CONN_CONNECTING      1U
#define PLC_WIFI_CONN_CONNECTED       2U
#define PLC_WIFI_CONN_GOT_IP          3U
#define PLC_WIFI_CONN_AUTH_FAILED     4U
#define PLC_WIFI_CONN_SSID_NOT_FOUND  5U
#define PLC_WIFI_CONN_AP_MODE_ACTIVE  6U

/* scan_state (0x1341) */
#define PLC_WIFI_SCAN_IDLE            0U
#define PLC_WIFI_SCAN_SCANNING        1U
#define PLC_WIFI_SCAN_COMPLETED       2U
#define PLC_WIFI_SCAN_FAILED          3U

/* command (0x1340) */
#define PLC_WIFI_CMD_IDLE             0U
#define PLC_WIFI_CMD_START_SCAN       1U
#define PLC_WIFI_CMD_CONNECT          2U
#define PLC_WIFI_CMD_DISCONNECT       3U
#define PLC_WIFI_CMD_FORGET           4U

/* error_code (0x1343), and the return value of the host's command callbacks */
#define PLC_WIFI_ERR_NONE             0
#define PLC_WIFI_ERR_BUSY             1
#define PLC_WIFI_ERR_TIMEOUT          2
#define PLC_WIFI_ERR_DRIVER           3
#define PLC_WIFI_ERR_HW_NOT_READY     4

/* ---- Host-facing types (plain C strings, host byte order) ---------------- */

typedef struct {
    uint16_t control_flags;       /* PLC_WIFI_CTRL_* */
    char     ssid[33];            /* NUL-terminated, up to 32 characters */
    char     password[65];        /* NUL-terminated, up to 64 characters */
    uint32_t static_ip;           /* 192.168.1.100 = 0xC0A80164; used when DHCP bit = 0 */
    uint32_t static_subnet;
    uint32_t static_gateway;
    uint32_t static_dns;
} splc_wifi_config_t;

typedef struct {
    uint16_t conn_status;         /* PLC_WIFI_CONN_* */
    int16_t  rssi_dbm;            /* the library shows -100 unless connected / AP mode */
    uint32_t ip;                  /* current IPv4, host byte order */
    uint8_t  mac[6];
    uint16_t scan_state;          /* PLC_WIFI_SCAN_* */
    uint16_t ap_count;            /* networks found (the library clamps to 8) */
    uint16_t error_code;          /* host's own error; 0 = none */
} splc_wifi_status_t;

typedef struct {
    char     ssid[33];
    int16_t  rssi_dbm;
    uint8_t  auth_mode;           /* 0 open, 1 WEP, 2 WPA, 3 WPA2, 4 WPA/WPA2, 5 WPA2-ENT, 6 WPA3 */
    uint8_t  channel;
    uint8_t  bssid[6];
} splc_wifi_ap_t;

/*
 * Host functions. connect, disconnect and status are REQUIRED; the rest is optional.
 * None may block (no wait for the radio): start the work, return, let the host's task
 * finish it, and report progress through status().
 *
 * int-returning callbacks: 0 = accepted, else an ICD error code (1 busy, 3 driver
 * error, 4 hardware not ready); any other value is treated as 3.
 */
typedef struct {
    void *ctx;

    /* Start connecting with this configuration (ssid is never empty here). */
    int  (*connect)(void *ctx, const splc_wifi_config_t *cfg);
    /* Leave the network. */
    int  (*disconnect)(void *ctx);
    /* Fill `out` from the host's cached live state. false = no state yet. */
    bool (*status)(void *ctx, splc_wifi_status_t *out);

    /* Optional: start a background scan (without it START_SCAN answers error 4). */
    int  (*scan_start)(void *ctx);
    /* Optional: result number `index` (0..7) of the last scan; false = no such entry. */
    bool (*scan_ap)(void *ctx, uint8_t index, splc_wifi_ap_t *out);

    /* Optional persistence (decision 50c). The library never writes Flash itself:
     *   load  - once after registration: the stored credentials, false = none stored.
     *           With ENABLE and AUTO_RECONNECT set the library then connects by itself
     *           (CHKL-WIFI-08);
     *   save  - called once after a CONNECT the App sent reached GOT_IP, so a wrong
     *           password never overwrites a good record;
     *   erase - FORGET and FACTORY_RESET. */
    bool (*load)(void *ctx, splc_wifi_config_t *out);
    bool (*save)(void *ctx, const splc_wifi_config_t *cfg);
    void (*erase)(void *ctx);
} splc_wifi_ops_t;

typedef enum {
    PLC_WIFI_WRITE_OK = 0,
    PLC_WIFI_WRITE_ADDR,          /* -> Modbus exception 0x02 */
    PLC_WIFI_WRITE_VALUE,         /* -> Modbus exception 0x03 */
} plc_wifi_write_result_t;

#if SPLC_WIFI_ENABLE

#ifdef __cplusplus
extern "C" {
#endif

/* Register the host functions (copied). Call from the engine context, before or after
 * plc_engine_init(). Returns false (and logs why) when a required callback is NULL. */
bool splc_wifi_register(const splc_wifi_ops_t *ops);
void splc_wifi_unregister(void);

/* FC03 of 0x1300..0x13FF; `offset` is relative to PLC_WIFI_BASE. */
void plc_wifi_read(uint16_t offset, uint16_t quantity, uint16_t *registers_out);

/* FC16/FC06 of 0x1300..0x13FF. All-or-nothing: a request that touches the read-only scan
 * table (offset >= 0x50) or carries an unknown command value changes nothing. */
plc_wifi_write_result_t plc_wifi_write(uint16_t offset, uint16_t quantity, const uint16_t *registers);

/* Once per scan (after modbus_config_service()): boot-time restore and the save after
 * GOT_IP. Cheap: polls the host at most every 100 ms and only while a save is pending. */
void plc_wifi_service(uint32_t now_ms);

/* FACTORY_RESET: erase the stored credentials and clear the RAM config. The running
 * connection is NOT dropped (the App's reply must still get out). */
void plc_wifi_factory_reset(void);

/* PLC_WIFI_CAP_WIFI when ops are registered, else 0. */
uint32_t plc_wifi_capabilities(void);

#ifdef __cplusplus
}
#endif

#else  /* SPLC_WIFI_ENABLE == 0: no code, no data */

static inline void     plc_wifi_service(uint32_t now_ms) { (void)now_ms; }
static inline void     plc_wifi_factory_reset(void) {}
static inline uint32_t plc_wifi_capabilities(void) { return 0UL; }

#endif /* SPLC_WIFI_ENABLE */

#endif /* PLC_WIFI_H */