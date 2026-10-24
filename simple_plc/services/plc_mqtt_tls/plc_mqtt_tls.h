#ifndef PLC_MQTT_TLS_H
#define PLC_MQTT_TLS_H

/*
 * plc_mqtt_tls.h - Layer 3 (PLC Application Services)
 *
 * MQTT TLS certificate block 0x0F00..0x0F43 of Wire V2.0 (ICD SPLC-SPEC-MQTT-003).
 *
 *   0x0F00..0x0F3F  staging buffer   64 regs  W   one SPLC_CertChunk_t (128 B)
 *   0x0F40          ACK chunk index   1 reg   R   index of the last chunk accepted (0xFFFF = none yet)
 *   0x0F41          STATUS            1 reg   R   0 OK, 1 chunk CRC, 2 overflow, 3 Flash write
 *   0x0F42          COMMIT            1 reg   W   0xA5A5 (reads 0)
 *   0x0F43          ACTIVE MASK       1 reg   R   bit0 Root CA, bit1 client cert, bit2 client key
 *
 * Chunk image (registers big-endian, payload two bytes per register, high byte first):
 *   reg 0  cert_type << 8 | chunk_index          (1 Root CA, 2 Client Cert, 3 Client Key)
 *   reg 1  total_chunks << 8 | chunk_len         (total_chunks 1..32, chunk_len 1..120)
 *   reg 2  total_cert_size                       (bytes)
 *   reg 3  chunk_crc16                           (CRC-16/MODBUS of the chunk_len payload bytes ONLY)
 *   reg 4..63  payload[120]
 *
 * Same philosophy as MQTT and Wi-Fi (decision 31): the library owns no TLS stack and
 * NO Flash for certificates and does NOT buffer them (the Gateway has ~3 KB of RAM to
 * spare, a certificate is up to 3 840 B). It checks every chunk and passes it straight to
 * the host through splc_mqtt_tls_ops_t; the host keeps the bytes (Flash or RAM) and loads
 * them into mbedTLS when it gets commit().
 *
 * What the library checks (anything wrong -> Modbus exception 0x03, nothing changes):
 *   header  cert_type 1..3, total_chunks 1..32, chunk_index < total_chunks,
 *           chunk_len 1..120 and <= total_cert_size,
 *           total_chunks <= total_cert_size <= total_chunks * 120;
 *   order   chunk 0 starts (or restarts) a transfer; every other chunk must belong to the
 *           open transfer (same type / total_chunks / total_cert_size) and be exactly the
 *           next one; the bytes must not exceed total_cert_size;
 *   resend  the chunk just acknowledged, again with the same CRC and length (a lost ACK),
 *           is acknowledged again and NOT passed to the host a second time; the same
 *           index with another CRC or length is rejected;
 *   COMMIT  0xA5A5 only, a transfer must be open and complete (all chunks, exact byte
 *           count); otherwise 0x03 and the transfer stays open.
 * A wrong chunk CRC does not raise an exception: ACK stays, STATUS = 1, the transfer stays
 * open and the App retries the chunk (ICD section 5). A failure reported by the host
 * (begin/chunk/commit return != 0) sets STATUS = 2 (return value 2) or 3 (anything else),
 * leaves ACK alone, abandons the transfer (host abort) and keeps the old certificate.
 * STATUS returns to 0 with the next good chunk or good COMMIT.
 * A transfer idle for 30 s is abandoned (host abort), ICD section 5.3.
 *
 * Declared in HARDWARE_CAPABILITIES (0x0A08 bit 0x0080) and accepted for use_tls=1 at
 * the broker block only once the host registered its functions.
 *
 * Context rule: every function runs in the engine task, and so do the host callbacks;
 * they must return at once (writing 120 bytes to RAM, or to a Flash buffer the host
 * erases/commits in its own task).
 * Compiled to nothing when SPLC_MQTT_TLS_CERT_ENABLE = 0.
 */

#include <stdint.h>
#include <stdbool.h>
#include "splc_opt.h"

#define PLC_MQTT_TLS_BASE             0x0F00U
#define PLC_MQTT_TLS_REGS             68U          /* 0x0F00..0x0F43 */
#define PLC_MQTT_TLS_STAGING_REGS     64U
#define PLC_MQTT_TLS_REG_ACK          64U          /* offsets relative to PLC_MQTT_TLS_BASE */
#define PLC_MQTT_TLS_REG_STATUS       65U
#define PLC_MQTT_TLS_REG_COMMIT       66U
#define PLC_MQTT_TLS_REG_MASK         67U

#define PLC_MQTT_TLS_COMMIT_MAGIC     0xA5A5U
#define PLC_MQTT_TLS_CHUNK_BYTES      120U
#define PLC_MQTT_TLS_MAX_CHUNKS       32U
#define PLC_MQTT_TLS_ABORT_MS         30000UL

#define PLC_MQTT_TLS_TYPE_ROOT_CA     1U
#define PLC_MQTT_TLS_TYPE_CLIENT_CERT 2U
#define PLC_MQTT_TLS_TYPE_CLIENT_KEY  3U

/* STATUS (0x0F41); also the meaning of a host callback's return value. */
#define PLC_MQTT_TLS_ST_OK            0U
#define PLC_MQTT_TLS_ST_CRC           1U
#define PLC_MQTT_TLS_ST_OVERFLOW      2U
#define PLC_MQTT_TLS_ST_FLASH         3U

/* Declared in HARDWARE_CAPABILITIES (0x0A08) once the host registered its functions. */
#define PLC_MQTT_TLS_CAP_CERT         0x00000080UL

typedef enum {
    PLC_MQTT_TLS_WRITE_OK = 0,
    PLC_MQTT_TLS_WRITE_ADDR,      /* -> Modbus exception 0x02 */
    PLC_MQTT_TLS_WRITE_VALUE,     /* -> Modbus exception 0x03 */
} plc_mqtt_tls_write_result_t;

/*
 * Host functions. begin, chunk, commit, abort and active_mask are REQUIRED, clear is optional.
 * int-returning callbacks: 0 = done; PLC_MQTT_TLS_ST_OVERFLOW (2) = no room; any other
 * value = storage error (reported as STATUS 3).
 */
typedef struct {
    void *ctx;

    /* chunk 0 arrived: a new certificate of `type` starts, `total_chunks` chunks,
     * `total_size` bytes. Discard any half-received data of the previous transfer. */
    int  (*begin)(void *ctx, uint8_t type, uint8_t total_chunks, uint16_t total_size);
    /* the next chunk, in order, CRC already checked. */
    int  (*chunk)(void *ctx, uint8_t type, uint8_t index, const uint8_t *data, uint8_t len);
    /* all chunks received and COMMIT 0xA5A5: store it for good and load it. On failure the
     * previous certificate must stay valid. */
    int  (*commit)(void *ctx, uint8_t type);
    /* the transfer was abandoned (restart, host failure, 30 s without a chunk). */
    void (*abort)(void *ctx);
    /* Optional: erase all stored certificates (FACTORY_RESET). */
    void (*clear)(void *ctx);
    /* bit0 Root CA, bit1 client cert, bit2 client key: which certificates are active. */
    uint16_t (*active_mask)(void *ctx);
} splc_mqtt_tls_ops_t;

#if SPLC_MQTT_TLS_CERT_ENABLE

#ifdef __cplusplus
extern "C" {
#endif

/* Register the host functions (copied). Returns false (and logs why) when a required
 * callback is NULL. Call from the engine context. */
bool splc_mqtt_tls_register(const splc_mqtt_tls_ops_t *ops);
void splc_mqtt_tls_unregister(void);

/* FC03 of 0x0F00..0x0F43; `offset` is relative to PLC_MQTT_TLS_BASE. */
void plc_mqtt_tls_read(uint16_t offset, uint16_t quantity, uint16_t *registers_out);

/* FC16/FC06 of 0x0F00..0x0F43 (see the header comment for the rules). */
plc_mqtt_tls_write_result_t plc_mqtt_tls_write(uint16_t offset, uint16_t quantity, const uint16_t *registers);

/* Once per scan: abandons a transfer that was idle for PLC_MQTT_TLS_ABORT_MS. */
void plc_mqtt_tls_service(uint32_t now_ms);

/* FACTORY_RESET: abandon any transfer and call the host's clear(). CLEAR_RULES does not. */
void plc_mqtt_tls_factory_reset(void);

/* true once the host registered its functions: use_tls=1 is then accepted at COMMIT. */
bool plc_mqtt_tls_ready(void);

/* PLC_MQTT_TLS_CAP_CERT once registered, else 0. */
uint32_t plc_mqtt_tls_capabilities(void);

#ifdef __cplusplus
}
#endif

#else  /* SPLC_MQTT_TLS_CERT_ENABLE == 0: no code, no data */

static inline void     plc_mqtt_tls_service(uint32_t now_ms) { (void)now_ms; }
static inline void     plc_mqtt_tls_factory_reset(void) {}
static inline bool     plc_mqtt_tls_ready(void) { return false; }
static inline uint32_t plc_mqtt_tls_capabilities(void) { return 0UL; }

#endif /* SPLC_MQTT_TLS_CERT_ENABLE */

#endif /* PLC_MQTT_TLS_H */