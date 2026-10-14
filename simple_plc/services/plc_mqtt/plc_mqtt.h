#ifndef PLC_MQTT_H
#define PLC_MQTT_H

/*
 * plc_mqtt.h - Layer 3 (PLC Application Services)
 *
 * MQTT Publish configuration (docs/plc_mqtt_design.md). SimplePLC only does
 * the LOGIC and HOLDS the data: there is no network client in this library
 * (decision 31). This header covers step E.2 (the configuration blocks,
 * their draft + COMMIT cycle, the Modbus register access and the status
 * block), step E.3 (the Flash image: plc_mqtt_export/import, stored by
 * plc_rule_flash inside the Rule Table record) and step E.4 (the publish
 * scan: plc_mqtt_scan() decides WHEN a block fires and renders the payload;
 * the host's publish callback, registered with splc_mqtt_register(), does the
 * actual network send).
 *
 * Register map (Structs MQTT profile, splc_mqtt_wire.h):
 *   0x0C00 + i*96   (i = 0..3)  Telemetry block i, 96 registers   R/W
 *   0x0E00..0x0E53              Broker config, 84 registers       R/W
 *   0x0E54..0x0E5F              Broker status, 12 registers       RO
 *
 * Staging (same idea as plc_fb, Step 8b):
 *   Host writes do NOT touch the running config. They go to a DRAFT and each
 *   block the Host wrote (any register of it) is flagged. The running config
 *   -- what FC03 returns -- only changes at COMMIT:
 *     plc_mqtt_validate_draft() : checks the WHOLE draft; false = reject the
 *                                 COMMIT, nothing was changed.
 *     plc_mqtt_commit_draft()   : flagged blocks take their draft content,
 *                                 blocks NOT written this time KEEP the
 *                                 running config (unlike plc_fb).
 *     plc_mqtt_discard_draft()  : throw the draft away.
 *   The draft is also dropped by plc_mqtt_init() (boot) and
 *   plc_mqtt_clear_all() (CLEAR_RULES / FACTORY_RESET).
 *
 * Content errors are caught at COMMIT, not at write time: the broker region
 * and the telemetry blocks always ACK a write that stays inside the map.
 *
 * Wire format: every register is stored as the 16-bit word the Host sent
 * (High Word first for 32-bit values, two characters per register with the
 * first character in the high byte). Nothing depends on the MCU endianness.
 *
 * Threading: single-threaded, engine context only (decision 31). Every
 * splc_mqtt_* function below must be called from the task that runs
 * plc_engine_poll().
 *
 * This file is transport-agnostic: it does not include nanomodbus.h.
 * plc_modbus_cfg.c maps the write results onto Modbus exceptions.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "splc_opt.h"

#if SPLC_MQTT_ENABLE

#include "splc_mqtt_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PLC_MQTT_TELEMETRY_BASE      0x0C00U
#define PLC_MQTT_TELEMETRY_REGS      (SPLC_MQTT_MAX_TELEMETRY_BLOCKS * SPLC_MQTT_REGS_PER_BLOCK) /* 384 */
#define PLC_MQTT_BROKER_BASE         0x0E00U
#define PLC_MQTT_BROKER_CFG_REGS     84U     /* 0x0E00..0x0E53, writable */
#define PLC_MQTT_BROKER_REGS         96U     /* 0x0E00..0x0E5F, incl. 12 RO status registers */

/* ------------------------------------------------------------------ lifecycle */

/* Boot: every block DISABLED, broker "not configured", draft dropped,
 * generation 0, counters 0. Call once, before plc_rule_flash_load(). */
void plc_mqtt_init(void);

/* ------------------------------------------------------------------ Modbus access */

/* FC03 of `quantity` registers starting at `offset` (0..383) inside the
 * telemetry area. Returns the RUNNING config. offset + quantity must stay
 * inside the area (the caller clamps each pass). */
void plc_mqtt_telemetry_read(uint16_t offset, uint16_t quantity, uint16_t *registers_out);

/* FC03 inside the broker area, offset 0..95. Returns the RUNNING config with
 * the password registers (offset 68..83) read as 0, followed by the live
 * status registers (offset 84..95). */
void plc_mqtt_broker_read(uint16_t offset, uint16_t quantity, uint16_t *registers_out);

typedef enum {
    PLC_MQTT_WRITE_OK = 0,
    PLC_MQTT_WRITE_BAD_ADDRESS     /* -> Modbus 0x02 */
} plc_mqtt_write_result_t;

/* FC16 (FC06 arrives as quantity 1) into the DRAFT. A request that runs past
 * the end of the area is BAD_ADDRESS and changes nothing. No content check
 * here (see "Content errors" above). */
plc_mqtt_write_result_t plc_mqtt_telemetry_write(uint16_t offset, uint16_t quantity,
                                                 const uint16_t *registers);

/* Same for the broker area, offset 0..95. A write that covers the read-only
 * status registers (84..95) is accepted and those registers are skipped. */
plc_mqtt_write_result_t plc_mqtt_broker_write(uint16_t offset, uint16_t quantity,
                                              const uint16_t *registers);

/* ------------------------------------------------------------------ COMMIT */

/* Check the whole draft (blocks/broker the Host wrote this time; the rest of
 * the running config is not re-checked). true also when nothing was written.
 * Changes nothing. */
bool plc_mqtt_validate_draft(void);

/* Promote the draft to the running config (call only after
 * plc_mqtt_validate_draft() returned true). Clears the draft. */
void plc_mqtt_commit_draft(void);

/* Drop the draft and its flags; the running config is untouched. */
void plc_mqtt_discard_draft(void);

/* ------------------------------------------------------------------ Flash image (step E.3) */

/* Size of the image below: 4 telemetry blocks x 192 B + broker 192 B.
 * plc_rule_flash.c cross-checks it with SPLC_RULE_FLASH_MQTT_SIZE. */
#define PLC_MQTT_FLASH_SIZE   960U

/* Serialise the RUNNING config (never the draft) into `out`
 * (PLC_MQTT_FLASH_SIZE bytes): 4 x 96 telemetry registers, then 96 broker
 * registers (84 config + 12 zero), each register big-endian, exactly the
 * words FC03 would carry. The password IS stored (it lives in Flash in clear
 * text; reading it back over Modbus still returns 0). */
void plc_mqtt_export(uint8_t *out);

/* Load an image written by plc_mqtt_export(). The whole image is checked
 * with the same rules as COMMIT; if any block fails, NOTHING is applied
 * (config stays as it was) and false is returned. On success the running
 * config is replaced, `generation` moves if app_configured changed, and any
 * draft is dropped. Used at boot (before the engine runs) and to roll back a
 * failed CLEAR_RULES. */
bool plc_mqtt_import(const uint8_t *in);

/* CLEAR_RULES / FACTORY_RESET: every block DISABLED, broker "not configured"
 * (generation moves if app_configured was true), draft dropped. */
void plc_mqtt_clear_all(void);

/* ------------------------------------------------------------------ publish scan (step E.4) */

/*
 * Called once per engine scan cycle, right after output_scan() and NOT while
 * a diag session suspends the rule engine (decision Q4: the values are then
 * forced by the Host, not real data). now_ms = the scan's tick (unsigned
 * arithmetic, safe across the 32-bit rollover).
 *
 * Does nothing until a publish callback is registered AND the broker is
 * READY: status.flags has CONNECTED and status.generation == generation (the
 * connection was built from the config the library holds now). While not
 * ready nothing is queued: data from that time is dropped and the first
 * publish after the broker becomes ready carries the then-current values.
 *
 * At most ONE publish per call, blocks served round-robin. A block is active
 * when ENABLED and (en_tag_index == 0xFFFF or that tag != 0). It fires when
 *   - it has not published since it became active (boot, reconnect, a COMMIT
 *     that rewrote the block, enable tag going 0 -> non-zero): once, so the
 *     cloud gets a baseline value; or
 *   - ON_CHANGE: any bound tag differs from the value last published;
 *     COV: any bound tag moved by >= its threshold (ceil(deadband * 10^dec),
 *     fixed at COMMIT); PERIODIC: interval_ms elapsed since the last publish.
 * When the callback returns < 0 nothing is recorded as published and the
 * block is tried again after SPLC_MQTT_RETRY_MS.
 */
void plc_mqtt_scan(uint32_t now_ms);

/* Publish callback registered by the Host (netif_add style, decision 16).
 * Runs in the engine context and must NOT block. `payload` is NUL-terminated,
 * len = strlen(payload); the buffer is reused right after the call, so copy it
 * if it must outlive the call. Return 0 = accepted, < 0 = busy / failed (the
 * library retries later and loses nothing). */
typedef int (*splc_mqtt_publish_fn)(const char *topic, const char *payload, size_t len,
                                    uint8_t qos, bool retain, void *ctx);

/* Register (or replace) the callback. Call before plc_engine_init(); it
 * survives plc_mqtt_init(). Returns false for fn == NULL. */
bool splc_mqtt_register(splc_mqtt_publish_fn fn, void *ctx);

/* Payloads that could not be rendered (should stay 0: COMMIT checks the worst
 * case). Mainly for tests and diagnostics. */
uint32_t splc_mqtt_format_errors(void);

/* ------------------------------------------------------------------ host-facing API (docs/plc_mqtt_design.md section 4) */

typedef struct {                 /* the Host fills this; the library reflects it at 0x0E54.. */
    uint16_t flags;              /* bit0 CONNECTED, bit1 TLS_ACTIVE, bit2 AUTH_OK */
    uint16_t latency_ms;
    uint16_t last_error;         /* SPLC_MqttBrokerErrorCode_t */
    uint32_t generation;         /* generation of the config the connection was built with */
} splc_mqtt_status_t;

#define SPLC_MQTT_STATUS_CONNECTED   0x0001U
#define SPLC_MQTT_STATUS_TLS_ACTIVE  0x0002U
#define SPLC_MQTT_STATUS_AUTH_OK     0x0004U

void splc_mqtt_set_status(const splc_mqtt_status_t *st);

typedef struct {                 /* copy of the running broker config, NUL-terminated strings */
    char     host[SPLC_MQTT_BROKER_HOST_MAX_CHARS + 1U];
    char     client_id[SPLC_MQTT_BROKER_CRED_MAX_CHARS + 1U];
    char     username[SPLC_MQTT_BROKER_CRED_MAX_CHARS + 1U];
    char     password[SPLC_MQTT_BROKER_CRED_MAX_CHARS + 1U];
    uint16_t port;
    uint16_t keepalive_sec;
    bool     use_tls;
    bool     clean_session;
    uint8_t  version;            /* 0 = MQTT 3.1.1, 1 = MQTT 5.0 */
} splc_mqtt_broker_t;

/*
 * Copies the running broker config out. *app_configured is true when the App
 * sent a broker AND its Enabled bit is 1 AND the host name is not empty: the
 * port then uses this config instead of the gateway's own default.
 * *generation moves when app_configured flips (either way) or, while it is
 * true, when the broker content really changes (status registers excluded).
 * Any pointer may be NULL.
 */
void splc_mqtt_get_broker(splc_mqtt_broker_t *out, bool *app_configured, uint32_t *generation);

/* Fallback for the {device} token while client_id is empty. Truncated to 32
 * characters. NULL clears it. */
void splc_mqtt_set_default_device_name(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* SPLC_MQTT_ENABLE */

#endif /* PLC_MQTT_H */