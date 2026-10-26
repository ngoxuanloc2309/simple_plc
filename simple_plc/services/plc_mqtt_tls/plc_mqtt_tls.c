/*
 * plc_mqtt_tls.c - MQTT TLS certificate block 0x0F00..0x0F43 (see plc_mqtt_tls.h for the
 * design and the rules; ICD SPLC-SPEC-MQTT-003).
 *
 * The library keeps NO certificate bytes: one chunk is decoded into a 120-byte stack
 * buffer, checked, and handed to the host. Only the bookkeeping of the open transfer
 * (a few integers) lives here.
 */

#include "plc_mqtt_tls.h"

#if SPLC_MQTT_TLS_CERT_ENABLE

#include <string.h>
#include "sx_time.h"
#include "splc_log.h"
#include "plc_modbus_cfg.h"      /* crc16_modbus_update(): the same CRC-16/MODBUS the rule table uses */

static const char *TAG = "PLC_MQTT_TLS";

#define ACK_NONE   0xFFFFU

typedef struct {
    bool     active;             /* a transfer is open (chunk 0 accepted, not yet committed/abandoned) */
    uint8_t  type;
    uint8_t  total_chunks;
    uint16_t total_size;
    uint16_t received;           /* bytes accepted so far */
    uint8_t  next;               /* index of the next chunk expected */
    uint16_t last_crc;           /* of the chunk acknowledged last, for the resend rule */
    uint8_t  last_len;
    uint32_t last_ms;            /* tick of the last accepted chunk, for the 30 s watchdog */
} tls_xfer_t;

static splc_mqtt_tls_ops_t s_ops;
static bool                s_registered;
static tls_xfer_t          s_x;
static uint16_t            s_ack    = ACK_NONE;
static uint16_t            s_status = PLC_MQTT_TLS_ST_OK;

/* ------------------------------------------------------------------ helpers */

static void reset_state(void)
{
    memset(&s_x, 0, sizeof(s_x));
    s_ack    = ACK_NONE;
    s_status = PLC_MQTT_TLS_ST_OK;
}

/* Abandon the open transfer and tell the host (no-op when none is open). */
static void drop_transfer(void)
{
    if (s_x.active) {
        s_x.active = false;
        if (s_registered) {
            s_ops.abort(s_ops.ctx);
        }
    }
}

static uint16_t host_status(int rc)
{
    return (rc == (int)PLC_MQTT_TLS_ST_OVERFLOW) ? PLC_MQTT_TLS_ST_OVERFLOW : PLC_MQTT_TLS_ST_FLASH;
}

static void check_watchdog(uint32_t now_ms)
{
    /*
     * Signed difference, NOT unsigned: plc_mqtt_tls_service() is called with the tick sampled
     * at the START of the scan, but a chunk accepted in modbus_config_service() of the same
     * scan stamps last_ms with a later tick (a Flash/littlefs write takes milliseconds). With
     * an unsigned difference now_ms - last_ms then wraps to ~4e9 and the watchdog abandons
     * the transfer right after chunk 0. A negative difference means "not idle yet". Still
     * safe across the 32-bit tick wrap (elapsed time stays far below 2^31 ms).
     */
    if (s_x.active && (int32_t)(now_ms - s_x.last_ms) >= (int32_t)PLC_MQTT_TLS_ABORT_MS) {
        log_warn(TAG, "transfer abandoned: no chunk for %lu ms", (unsigned long)PLC_MQTT_TLS_ABORT_MS);
        drop_transfer();
    }
}

/* ------------------------------------------------------------------ registration */

bool splc_mqtt_tls_register(const splc_mqtt_tls_ops_t *ops)
{
    if (ops == NULL || ops->begin == NULL || ops->chunk == NULL || ops->commit == NULL ||
        ops->abort == NULL || ops->active_mask == NULL) {
        log_error(TAG, "register: begin, chunk, commit, abort and active_mask are required");
        return false;
    }
    drop_transfer();
    s_ops        = *ops;
    s_registered = true;
    reset_state();
    if (ops->clear == NULL) {
        log_warn(TAG, "no clear callback: FACTORY_RESET will not erase the stored certificates");
    }
    return true;
}

void splc_mqtt_tls_unregister(void)
{
    drop_transfer();
    s_registered = false;
    memset(&s_ops, 0, sizeof(s_ops));
    reset_state();
}

bool plc_mqtt_tls_ready(void)
{
    return s_registered;
}

uint32_t plc_mqtt_tls_capabilities(void)
{
    return s_registered ? PLC_MQTT_TLS_CAP_CERT : 0UL;
}

/* ------------------------------------------------------------------ read */

void plc_mqtt_tls_read(uint16_t offset, uint16_t quantity, uint16_t *registers_out)
{
    for (uint16_t i = 0; i < quantity; i++) {
        const uint16_t reg = (uint16_t)(offset + i);
        uint16_t v = 0U;                                   /* staging and COMMIT read 0 */
        if (reg == PLC_MQTT_TLS_REG_ACK) {
            v = s_ack;
        } else if (reg == PLC_MQTT_TLS_REG_STATUS) {
            v = s_status;
        } else if (reg == PLC_MQTT_TLS_REG_MASK && s_registered) {
            v = (uint16_t)(s_ops.active_mask(s_ops.ctx) & 0x0007U);
        }
        registers_out[i] = v;
    }
}

/* ------------------------------------------------------------------ chunk */

static plc_mqtt_tls_write_result_t write_chunk(const uint16_t *r)
{
    const uint8_t  type  = (uint8_t)(r[0] >> 8);
    const uint8_t  idx   = (uint8_t)(r[0] & 0xFFU);
    const uint8_t  total = (uint8_t)(r[1] >> 8);
    const uint8_t  len   = (uint8_t)(r[1] & 0xFFU);
    const uint16_t size  = r[2];
    const uint16_t crc   = r[3];

    if (!s_registered) {
        return PLC_MQTT_TLS_WRITE_ADDR;
    }
    if (type < PLC_MQTT_TLS_TYPE_ROOT_CA || type > PLC_MQTT_TLS_TYPE_CLIENT_KEY ||
        total < 1U || total > PLC_MQTT_TLS_MAX_CHUNKS || idx >= total ||
        len < 1U || len > PLC_MQTT_TLS_CHUNK_BYTES || len > size ||
        size < total || size > (uint32_t)total * PLC_MQTT_TLS_CHUNK_BYTES) {
        log_warn(TAG, "chunk rejected: bad header type=%u idx=%u total=%u len=%u size=%u",
                 type, idx, total, len, size);
        return PLC_MQTT_TLS_WRITE_VALUE;
    }

    uint8_t data[PLC_MQTT_TLS_CHUNK_BYTES];
    uint16_t calc = 0xFFFFU;
    for (uint8_t k = 0; k < len; k++) {
        const uint16_t reg = r[4U + (k >> 1)];
        data[k] = (uint8_t)((k & 1U) ? (reg & 0xFFU) : (reg >> 8));
        calc = crc16_modbus_update(calc, data[k]);          /* the chunk_len bytes only, not the padding */
    }
    if (calc != crc) {
        log_warn(TAG, "chunk %u: CRC mismatch (calc 0x%04X, got 0x%04X)", idx, calc, crc);
        s_status = PLC_MQTT_TLS_ST_CRC;                     /* ACK stays; the App resends */
        return PLC_MQTT_TLS_WRITE_OK;
    }

    if (idx == 0U) {
        drop_transfer();                                    /* a restart: the host forgets the old one */
        int rc = s_ops.begin(s_ops.ctx, type, total, size);
        if (rc != 0) {
            log_warn(TAG, "host begin() failed: %d", rc);
            s_status = host_status(rc);
            return PLC_MQTT_TLS_WRITE_OK;
        }
        s_x.active       = true;
        s_x.type         = type;
        s_x.total_chunks = total;
        s_x.total_size   = size;
        s_x.received     = 0U;
        s_x.next         = 0U;
    } else {
        if (!s_x.active || type != s_x.type || total != s_x.total_chunks || size != s_x.total_size) {
            log_warn(TAG, "chunk %u rejected: no open transfer or it does not match", idx);
            return PLC_MQTT_TLS_WRITE_VALUE;
        }
        if (s_x.next >= 1U && idx == (uint8_t)(s_x.next - 1U)) {
            if (crc == s_x.last_crc && len == s_x.last_len) {
                s_x.last_ms = sx_get_tick_ms();             /* lost ACK: answer again, do not re-send to the host */
                s_ack       = idx;
                s_status    = PLC_MQTT_TLS_ST_OK;
                return PLC_MQTT_TLS_WRITE_OK;
            }
            log_warn(TAG, "chunk %u rejected: resend with different content", idx);
            return PLC_MQTT_TLS_WRITE_VALUE;
        }
        if (idx != s_x.next || (uint32_t)s_x.received + len > s_x.total_size) {
            log_warn(TAG, "chunk %u rejected: expected %u (or it overruns the size)", idx, s_x.next);
            return PLC_MQTT_TLS_WRITE_VALUE;
        }
    }

    int rc = s_ops.chunk(s_ops.ctx, type, idx, data, len);
    if (rc != 0) {
        log_warn(TAG, "host chunk(%u) failed: %d", idx, rc);
        s_status = host_status(rc);
        drop_transfer();                                    /* host abort; the old certificate stays */
        return PLC_MQTT_TLS_WRITE_OK;
    }
    s_x.received = (uint16_t)(s_x.received + len);
    s_x.next     = (uint8_t)(idx + 1U);
    s_x.last_crc = crc;
    s_x.last_len = len;
    s_x.last_ms  = sx_get_tick_ms();
    s_ack        = idx;
    s_status     = PLC_MQTT_TLS_ST_OK;
    return PLC_MQTT_TLS_WRITE_OK;
}

/* ------------------------------------------------------------------ commit */

static plc_mqtt_tls_write_result_t write_commit(uint16_t value)
{
    if (!s_registered) {
        return PLC_MQTT_TLS_WRITE_ADDR;
    }
    if (value != PLC_MQTT_TLS_COMMIT_MAGIC) {
        log_warn(TAG, "commit rejected: value 0x%04X is not 0x%04X", value, PLC_MQTT_TLS_COMMIT_MAGIC);
        return PLC_MQTT_TLS_WRITE_VALUE;
    }
    if (!s_x.active || s_x.next != s_x.total_chunks || s_x.received != s_x.total_size) {
        log_warn(TAG, "commit rejected: transfer %s (chunks %u/%u, bytes %u/%u)",
                 s_x.active ? "incomplete" : "not open", s_x.next, s_x.total_chunks,
                 s_x.received, s_x.total_size);
        return PLC_MQTT_TLS_WRITE_VALUE;                    /* an open transfer stays open */
    }
    const int rc = s_ops.commit(s_ops.ctx, s_x.type);
    if (rc != 0) {
        log_warn(TAG, "host commit() failed: %d; the previous certificate stays", rc);
        s_status = host_status(rc);
        drop_transfer();
        return PLC_MQTT_TLS_WRITE_OK;
    }
    s_x.active = false;                                     /* committed: nothing left to abort */
    s_status   = PLC_MQTT_TLS_ST_OK;
    log_info(TAG, "certificate type %u committed (%u bytes)", s_x.type, s_x.total_size);
    return PLC_MQTT_TLS_WRITE_OK;
}

/* ------------------------------------------------------------------ write */

plc_mqtt_tls_write_result_t plc_mqtt_tls_write(uint16_t offset, uint16_t quantity, const uint16_t *registers)
{
    if (quantity == 0U || (uint32_t)offset + quantity > PLC_MQTT_TLS_REGS) {
        return PLC_MQTT_TLS_WRITE_ADDR;                     /* past 0x0F43 */
    }
    check_watchdog(sx_get_tick_ms());

    if (offset < PLC_MQTT_TLS_STAGING_REGS) {
        if ((uint32_t)offset + quantity > PLC_MQTT_TLS_STAGING_REGS) {
            return PLC_MQTT_TLS_WRITE_ADDR;                 /* runs into ACK/STATUS (read-only) */
        }
        if (offset != 0U || quantity != PLC_MQTT_TLS_STAGING_REGS) {
            log_warn(TAG, "write of %u registers at +%u: a chunk is exactly 64 registers", quantity, offset);
            return PLC_MQTT_TLS_WRITE_VALUE;
        }
        return write_chunk(registers);
    }
    if (offset == PLC_MQTT_TLS_REG_COMMIT && quantity == 1U) {
        return write_commit(registers[0]);
    }
    return PLC_MQTT_TLS_WRITE_ADDR;                         /* ACK, STATUS, MASK read-only; COMMIT is one register */
}

/* ------------------------------------------------------------------ service / reset */

void plc_mqtt_tls_service(uint32_t now_ms)
{
    check_watchdog(now_ms);
}

void plc_mqtt_tls_factory_reset(void)
{
    drop_transfer();
    if (s_registered && s_ops.clear != NULL) {
        s_ops.clear(s_ops.ctx);
    }
    reset_state();
}

#endif /* SPLC_MQTT_TLS_CERT_ENABLE */