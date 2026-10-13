#include "plc_mqtt.h"

#if SPLC_MQTT_ENABLE

#include <string.h>

#include "plc_tag.h"          /* MAX_TAGS */
#include "splc_mqtt_fmt.h"    /* template check, deadband check */
#include "splc_log.h"

/*
 * plc_mqtt.c - Layer 3. See plc_mqtt.h for the contract and
 * docs/plc_mqtt_design.md sections 5 (registers, draft, COMMIT) and 7..8.
 *
 * Storage is the register image itself (16-bit words exactly as the Host
 * sent them), so FC03 is a plain copy and nothing depends on the MCU
 * endianness or struct padding. The wire struct in splc_mqtt_wire.h is the
 * documentation of the layout; the offsets below must match it.
 */

static const char *TAG = "PLC_MQTT";

/* ---- Telemetry block layout (registers inside one 96-register block) ---- */
#define TB_CONTROL      0U
#define TB_EN_TAG       1U
#define TB_INTERVAL_HI  2U
#define TB_INTERVAL_LO  3U
#define TB_DEADBAND_HI  4U
#define TB_DEADBAND_LO  5U
#define TB_TAG_COUNT    6U
#define TB_TAGS         7U     /* 8 registers */
#define TB_RESERVED     15U
#define TB_TOPIC        16U    /* 16 registers = 32 chars */
#define TB_TEMPLATE     32U    /* 64 registers = 128 chars */

#define TB_TOPIC_CHARS  SPLC_MQTT_MAX_TOPIC_CHARS
#define TB_TMPL_CHARS   SPLC_MQTT_MAX_TEMPLATE_CHARS

/* ---- Broker layout (registers inside the broker area) ---- */
#define BK_CONTROL      0U
#define BK_PORT         1U
#define BK_KEEPALIVE    2U
#define BK_RESERVED     3U
#define BK_HOST         4U     /* 32 registers = 64 chars */
#define BK_CLIENT_ID    36U    /* 16 registers = 32 chars */
#define BK_USERNAME     52U
#define BK_PASSWORD     68U
#define BK_STATUS       84U    /* first read-only register */
#define BK_ST_FLAGS     84U
#define BK_ST_LATENCY   85U
#define BK_ST_TXMSG_HI  86U
#define BK_ST_TXMSG_LO  87U
#define BK_ST_TXBYTE_HI 88U
#define BK_ST_TXBYTE_LO 89U
#define BK_ST_LAST_ERR  90U

_Static_assert(SPLC_MQTT_REGS_PER_BLOCK == 96U, "telemetry block layout assumes 96 registers");
_Static_assert(SPLC_MQTT_BROKER_CONFIG_REGS == 96U, "broker layout assumes 96 registers");
_Static_assert(TB_TEMPLATE + (TB_TMPL_CHARS / 2U) == SPLC_MQTT_REGS_PER_BLOCK,
               "telemetry block layout must end exactly at register 96");
_Static_assert(BK_PASSWORD + (SPLC_MQTT_BROKER_CRED_MAX_CHARS / 2U) == BK_STATUS,
               "password must be the last writable broker field");
_Static_assert(PLC_MQTT_BROKER_CFG_REGS == BK_STATUS, "writable broker registers end at the status block");

#define BLOCKS          SPLC_MQTT_MAX_TELEMETRY_BLOCKS
#define BLOCK_REGS      SPLC_MQTT_REGS_PER_BLOCK

/* Running config: what FC03 returns. */
static uint16_t s_tel[BLOCKS][BLOCK_REGS];
static uint16_t s_brk[PLC_MQTT_BROKER_CFG_REGS];

/* Draft: meaningful only while the block's bit is set in s_draft_mask
 * (bit i = telemetry block i, bit BLOCKS = broker). */
static uint16_t s_tel_draft[BLOCKS][BLOCK_REGS];
static uint16_t s_brk_draft[PLC_MQTT_BROKER_CFG_REGS];
static uint8_t  s_draft_mask;
#define DRAFT_BROKER_BIT   (1U << BLOCKS)

/* Broker bookkeeping (docs/plc_mqtt_design.md decision 31). */
static bool     s_app_configured;
static uint32_t s_generation;

/* Host-reported status, reflected at 0x0E54.. */
static splc_mqtt_status_t s_status;

/* Message / byte counters (+86..89). Updated by the publish scan (step E.4). */
static uint32_t s_tx_msgs;
static uint32_t s_tx_bytes;

/* Fallback for {device} while client_id is empty (used by the scan, E.4). */
static char s_default_device[SPLC_MQTT_BROKER_CRED_MAX_CHARS + 1U];

/* ------------------------------------------------------------------ helpers */

static uint32_t reg32(const uint16_t *r, uint16_t hi_idx)
{
    return ((uint32_t)r[hi_idx] << 16) | (uint32_t)r[hi_idx + 1U];
}

/* Copy `nchars` characters (two per register, first one in the high byte)
 * into out[0..nchars-1] and terminate with NUL at out[nchars]. The string
 * ends at the first NUL; bytes after it are ignored. */
static void regs_to_str(const uint16_t *r, size_t nchars, char *out)
{
    size_t i;
    for (i = 0; i < nchars; i++) {
        uint16_t w = r[i / 2U];
        char     c = (char)((i % 2U) == 0U ? (w >> 8) : (w & 0xFFU));
        out[i] = c;
        if (c == '\0') {
            return;
        }
    }
    out[nchars] = '\0';
}

static bool broker_enabled(const uint16_t *brk)
{
    return (brk[BK_CONTROL] & SPLC_BROKER_CTRL_ENABLED_BIT) != 0U;
}

static bool broker_host_empty(const uint16_t *brk)
{
    return (brk[BK_HOST] >> 8) == 0U;     /* first character is NUL */
}

/* app_configured = sent by the App AND Enabled=1 AND host not empty. "Sent by
 * the App" needs no extra flag: an image that was never written is all zero,
 * so Enabled is 0. */
static bool broker_app_configured(const uint16_t *brk)
{
    return broker_enabled(brk) && !broker_host_empty(brk);
}

/* ------------------------------------------------------------------ lifecycle */

void plc_mqtt_init(void)
{
    memset(s_tel, 0, sizeof(s_tel));
    memset(s_brk, 0, sizeof(s_brk));
    memset(s_tel_draft, 0, sizeof(s_tel_draft));
    memset(s_brk_draft, 0, sizeof(s_brk_draft));
    memset(&s_status, 0, sizeof(s_status));
    s_draft_mask     = 0U;
    s_app_configured = false;
    s_generation     = 0U;
    s_tx_msgs        = 0U;
    s_tx_bytes       = 0U;
    s_default_device[0] = '\0';
}

/* ------------------------------------------------------------------ Modbus read */

void plc_mqtt_telemetry_read(uint16_t offset, uint16_t quantity, uint16_t *registers_out)
{
    for (uint16_t i = 0; i < quantity; i++) {
        uint16_t idx = (uint16_t)(offset + i);
        registers_out[i] = (idx < PLC_MQTT_TELEMETRY_REGS)
                               ? s_tel[idx / BLOCK_REGS][idx % BLOCK_REGS]
                               : 0U;
    }
}

static uint16_t broker_reg_now(uint16_t idx)
{
    if (idx < BK_PASSWORD) {
        return s_brk[idx];
    }
    if (idx < BK_STATUS) {
        return 0U;                          /* the password never leaves the device */
    }
    switch (idx) {
        case BK_ST_FLAGS:     return s_status.flags;
        case BK_ST_LATENCY:   return s_status.latency_ms;
        case BK_ST_TXMSG_HI:  return (uint16_t)(s_tx_msgs >> 16);
        case BK_ST_TXMSG_LO:  return (uint16_t)(s_tx_msgs & 0xFFFFU);
        case BK_ST_TXBYTE_HI: return (uint16_t)(s_tx_bytes >> 16);
        case BK_ST_TXBYTE_LO: return (uint16_t)(s_tx_bytes & 0xFFFFU);
        case BK_ST_LAST_ERR:  return s_status.last_error;
        default:              return 0U;    /* +91..95 reserved */
    }
}

void plc_mqtt_broker_read(uint16_t offset, uint16_t quantity, uint16_t *registers_out)
{
    for (uint16_t i = 0; i < quantity; i++) {
        uint16_t idx = (uint16_t)(offset + i);
        registers_out[i] = (idx < PLC_MQTT_BROKER_REGS) ? broker_reg_now(idx) : 0U;
    }
}

/* ------------------------------------------------------------------ Modbus write (to the draft) */

plc_mqtt_write_result_t plc_mqtt_telemetry_write(uint16_t offset, uint16_t quantity,
                                                 const uint16_t *registers)
{
    if ((uint32_t)offset + quantity > PLC_MQTT_TELEMETRY_REGS) {
        log_warn(TAG, "telemetry write rejected: offset=%u quantity=%u runs past 0x0D7F",
                 offset, quantity);
        return PLC_MQTT_WRITE_BAD_ADDRESS;
    }
    for (uint16_t i = 0; i < quantity; i++) {
        uint16_t idx = (uint16_t)(offset + i);
        uint16_t blk = idx / BLOCK_REGS;
        if ((s_draft_mask & (1U << blk)) == 0U) {
            /* First write into this block: start from the running config so a
             * single-register write keeps the block's other fields. */
            memcpy(s_tel_draft[blk], s_tel[blk], sizeof(s_tel_draft[blk]));
            s_draft_mask |= (uint8_t)(1U << blk);
        }
        s_tel_draft[blk][idx % BLOCK_REGS] = registers[i];
    }
    return PLC_MQTT_WRITE_OK;
}

plc_mqtt_write_result_t plc_mqtt_broker_write(uint16_t offset, uint16_t quantity,
                                              const uint16_t *registers)
{
    if ((uint32_t)offset + quantity > PLC_MQTT_BROKER_REGS) {
        log_warn(TAG, "broker write rejected: offset=%u quantity=%u runs past 0x0E5F",
                 offset, quantity);
        return PLC_MQTT_WRITE_BAD_ADDRESS;
    }
    for (uint16_t i = 0; i < quantity; i++) {
        uint16_t idx = (uint16_t)(offset + i);
        if (idx >= BK_STATUS) {
            continue;                       /* read-only status: accepted, ignored */
        }
        if ((s_draft_mask & DRAFT_BROKER_BIT) == 0U) {
            memcpy(s_brk_draft, s_brk, sizeof(s_brk_draft));
            s_draft_mask |= (uint8_t)DRAFT_BROKER_BIT;
        }
        s_brk_draft[idx] = registers[i];
    }
    return PLC_MQTT_WRITE_OK;
}

/* ------------------------------------------------------------------ validation (COMMIT time) */

static bool topic_valid(const uint16_t *blk)
{
    char topic[TB_TOPIC_CHARS + 1U];
    regs_to_str(&blk[TB_TOPIC], TB_TOPIC_CHARS, topic);

    if (topic[0] == '\0' || topic[0] == '/') {
        return false;
    }
    for (size_t i = 0; topic[i] != '\0'; i++) {
        unsigned char c = (unsigned char)topic[i];
        if (c < 0x20U || c > 0x7EU || c == '+' || c == '#') {
            return false;
        }
    }
    return true;
}

static bool block_valid(const uint16_t *blk, unsigned index)
{
    const uint16_t ctrl = blk[TB_CONTROL];

    if (((ctrl & SPLC_MQTT_CTRL_MODE_MASK) >> SPLC_MQTT_CTRL_MODE_SHIFT) > SPLC_MQTT_MODE_PERIODIC) {
        log_warn(TAG, "COMMIT: block %u: bad MODE", index);
        return false;
    }
    if (((ctrl & SPLC_MQTT_CTRL_QOS_MASK) >> SPLC_MQTT_CTRL_QOS_SHIFT) > SPLC_MQTT_QOS_2) {
        log_warn(TAG, "COMMIT: block %u: bad QOS", index);
        return false;
    }
    const unsigned payload = (ctrl & SPLC_MQTT_CTRL_PAYLOAD_MASK) >> SPLC_MQTT_CTRL_PAYLOAD_SHIFT;
    if (payload > SPLC_MQTT_PAYLOAD_CUSTOM) {
        log_warn(TAG, "COMMIT: block %u: bad PAYLOAD mode", index);
        return false;
    }

    if ((ctrl & SPLC_MQTT_CTRL_ENABLED_BIT) == 0U) {
        return true;                        /* disabled: nothing else is checked */
    }

    const uint16_t tag_count = blk[TB_TAG_COUNT];
    if (tag_count < 1U || tag_count > SPLC_MQTT_MAX_TAGS_PER_BLOCK) {
        log_warn(TAG, "COMMIT: block %u: tag_count=%u (need 1..8)", index, tag_count);
        return false;
    }
    for (unsigned k = 0; k < SPLC_MQTT_MAX_TAGS_PER_BLOCK; k++) {
        const uint16_t t = blk[TB_TAGS + k];
        if (k < tag_count) {
            if (t >= MAX_TAGS) {
                log_warn(TAG, "COMMIT: block %u: bound tag %u = %u >= MAX_TAGS", index, k, t);
                return false;
            }
        } else if (t != 0xFFFFU) {
            log_warn(TAG, "COMMIT: block %u: unused tag slot %u must be 0xFFFF", index, k);
            return false;
        }
    }
    if (blk[TB_EN_TAG] != 0xFFFFU && blk[TB_EN_TAG] >= MAX_TAGS) {
        log_warn(TAG, "COMMIT: block %u: en_tag_index=%u", index, blk[TB_EN_TAG]);
        return false;
    }
    if (((ctrl & SPLC_MQTT_CTRL_MODE_MASK) >> SPLC_MQTT_CTRL_MODE_SHIFT) == SPLC_MQTT_MODE_PERIODIC &&
        reg32(blk, TB_INTERVAL_HI) < SPLC_MQTT_MIN_INTERVAL_MS) {
        log_warn(TAG, "COMMIT: block %u: interval_ms %lu < %u", index,
                 (unsigned long)reg32(blk, TB_INTERVAL_HI), (unsigned)SPLC_MQTT_MIN_INTERVAL_MS);
        return false;
    }

    /* deadband: IEEE 754 float32, High Word first. NaN, Inf and negative are rejected;
     * a huge value is fine (the threshold is clamped to INT32_MAX). */
    {
        const uint32_t bits = reg32(blk, TB_DEADBAND_HI);
        float          deadband;
        uint32_t       threshold;
        memcpy(&deadband, &bits, sizeof(deadband));
        if (!splc_mqtt_fmt_deadband_threshold(deadband, 0U, &threshold)) {
            log_warn(TAG, "COMMIT: block %u: deadband is NaN/Inf/negative", index);
            return false;
        }
    }

    if (!topic_valid(blk)) {
        log_warn(TAG, "COMMIT: block %u: bad topic", index);
        return false;
    }

    if (payload == SPLC_MQTT_PAYLOAD_CUSTOM) {
        char tmpl[TB_TMPL_CHARS + 1U];
        regs_to_str(&blk[TB_TEMPLATE], TB_TMPL_CHARS, tmpl);
        const splc_mqtt_fmt_err_t e =
            splc_mqtt_fmt_check_template(tmpl, TB_TMPL_CHARS, (uint8_t)tag_count, SPLC_MQTT_PAYLOAD_MAX);
        if (e != SPLC_MQTT_FMT_OK) {
            log_warn(TAG, "COMMIT: block %u: bad payload template (error %d)", index, (int)e);
            return false;
        }
    }
    return true;
}

static bool broker_valid(const uint16_t *brk)
{
    if (!broker_app_configured(brk)) {
        return true;                        /* the port keeps the gateway default; nothing to check */
    }
    if (brk[BK_PORT] == 0U) {
        log_warn(TAG, "COMMIT: broker port is 0");
        return false;
    }
#if !SPLC_MQTT_TLS_SUPPORTED
    if ((brk[BK_CONTROL] & SPLC_BROKER_CTRL_USE_TLS_BIT) != 0U) {
        log_warn(TAG, "COMMIT: broker use_tls=1 but this build has no TLS (SPLC_MQTT_TLS_SUPPORTED=0)");
        return false;
    }
#endif
    if (((brk[BK_CONTROL] & SPLC_BROKER_CTRL_VERSION_MASK) >> SPLC_BROKER_CTRL_VERSION_SHIFT) > 1U) {
        log_warn(TAG, "COMMIT: broker protocol version is not 0 (3.1.1) or 1 (5.0)");
        return false;
    }
    return true;
}

bool plc_mqtt_validate_draft(void)
{
    for (unsigned i = 0; i < BLOCKS; i++) {
        if ((s_draft_mask & (1U << i)) != 0U && !block_valid(s_tel_draft[i], i)) {
            return false;
        }
    }
    if ((s_draft_mask & DRAFT_BROKER_BIT) != 0U && !broker_valid(s_brk_draft)) {
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ COMMIT / discard / clear */

/* Replace the running broker image and move `generation` when it matters. */
static void broker_apply(const uint16_t *image)
{
    const bool was = s_app_configured;
    const bool now = broker_app_configured(image);

    if (was != now) {
        s_generation++;
    } else if (now && memcmp(s_brk, image, sizeof(s_brk)) != 0) {
        s_generation++;                     /* same state, different content: reconnect needed */
    }
    memcpy(s_brk, image, sizeof(s_brk));
    s_app_configured = now;
}

void plc_mqtt_commit_draft(void)
{
    for (unsigned i = 0; i < BLOCKS; i++) {
        if ((s_draft_mask & (1U << i)) != 0U) {
            memcpy(s_tel[i], s_tel_draft[i], sizeof(s_tel[i]));
        }
    }
    if ((s_draft_mask & DRAFT_BROKER_BIT) != 0U) {
        broker_apply(s_brk_draft);
    }
    s_draft_mask = 0U;
    log_info(TAG, "COMMIT: MQTT config applied (app_configured=%d generation=%lu)",
             (int)s_app_configured, (unsigned long)s_generation);
}

void plc_mqtt_discard_draft(void)
{
    s_draft_mask = 0U;
}

void plc_mqtt_clear_all(void)
{
    static const uint16_t zero_brk[PLC_MQTT_BROKER_CFG_REGS];   /* all zero */

    memset(s_tel, 0, sizeof(s_tel));
    broker_apply(zero_brk);
    s_draft_mask = 0U;
}

/* ------------------------------------------------------------------ host-facing API */

void splc_mqtt_set_status(const splc_mqtt_status_t *st)
{
    if (st != NULL) {
        s_status = *st;
    }
}

void splc_mqtt_get_broker(splc_mqtt_broker_t *out, bool *app_configured, uint32_t *generation)
{
    if (out != NULL) {
        regs_to_str(&s_brk[BK_HOST],      SPLC_MQTT_BROKER_HOST_MAX_CHARS, out->host);
        regs_to_str(&s_brk[BK_CLIENT_ID], SPLC_MQTT_BROKER_CRED_MAX_CHARS, out->client_id);
        regs_to_str(&s_brk[BK_USERNAME],  SPLC_MQTT_BROKER_CRED_MAX_CHARS, out->username);
        regs_to_str(&s_brk[BK_PASSWORD],  SPLC_MQTT_BROKER_CRED_MAX_CHARS, out->password);
        out->port          = s_brk[BK_PORT];
        out->keepalive_sec = s_brk[BK_KEEPALIVE];
        out->use_tls       = (s_brk[BK_CONTROL] & SPLC_BROKER_CTRL_USE_TLS_BIT) != 0U;
        out->clean_session = (s_brk[BK_CONTROL] & SPLC_BROKER_CTRL_CLEAN_SESSION_BIT) != 0U;
        out->version       = (uint8_t)((s_brk[BK_CONTROL] & SPLC_BROKER_CTRL_VERSION_MASK) >>
                                       SPLC_BROKER_CTRL_VERSION_SHIFT);
    }
    if (app_configured != NULL) {
        *app_configured = s_app_configured;
    }
    if (generation != NULL) {
        *generation = s_generation;
    }
}

void splc_mqtt_set_default_device_name(const char *name)
{
    size_t i = 0;
    if (name != NULL) {
        for (; i < SPLC_MQTT_BROKER_CRED_MAX_CHARS && name[i] != '\0'; i++) {
            s_default_device[i] = name[i];
        }
    }
    s_default_device[i] = '\0';
}

#endif /* SPLC_MQTT_ENABLE */