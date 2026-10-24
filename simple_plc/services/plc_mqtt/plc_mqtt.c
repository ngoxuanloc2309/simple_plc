#include "plc_mqtt.h"

#if SPLC_MQTT_ENABLE

#include <string.h>

#include "plc_tag.h"          /* MAX_TAGS, tag_read() */
#include "plc_rtc.h"          /* plc_rtc_get_epoch(): {ts} */
#include "splc_mqtt_fmt.h"    /* template check, deadband check */
#include "splc_opt.h"         /* SPLC_REAL_ENABLE */
#include "plc_mqtt_tls.h"      /* plc_mqtt_tls_ready(): use_tls=1 is accepted once the host registered TLS */
#if SPLC_REAL_ENABLE
#include "plc_tagtype.h"      /* plc_tagtype_get(): REAL / BOOL / INT32 per tag */
#endif
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

/* Fallback for {device} while client_id is empty. */
static char s_default_device[SPLC_MQTT_BROKER_CRED_MAX_CHARS + 1U];

/* ---- publish scan state (step E.4). RAM only, rebuilt from the running
 * config whenever a block changes (rt_prepare). ---- */
typedef struct {
    int32_t  last_val[SPLC_MQTT_MAX_TAGS_PER_BLOCK]; /* values at the last accepted publish */
    uint32_t cov_thr[SPLC_MQTT_MAX_TAGS_PER_BLOCK];  /* COV threshold in raw tag units, per tag */
#if SPLC_REAL_ENABLE
    float    cov_real;        /* COV threshold for REAL tags: the deadband itself (engineering units) */
#endif
    uint32_t last_pub_ms;
    uint32_t retry_at_ms;
    bool     need_first;      /* publish once, regardless of mode (baseline value) */
    bool     was_active;      /* active at the previous scan (enable tag edge) */
    bool     retry_wait;      /* publish was refused: wait until retry_at_ms */
} block_rt_t;

static block_rt_t s_rt[BLOCKS];
static unsigned   s_rr;                 /* block the next scan looks at first */
static bool       s_was_ready;          /* broker was ready at the previous scan */
static uint32_t   s_format_errors;
static bool       s_format_warned;
#if SPLC_REAL_ENABLE
static bool       s_nonfinite_warned;
#endif
static splc_mqtt_publish_fn s_publish;  /* NOT cleared by plc_mqtt_init(): the Host registers before the engine starts */
static void      *s_publish_ctx;

static char s_payload[SPLC_MQTT_PAYLOAD_MAX];   /* one buffer shared by all blocks */

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

/* Decimal digits of bound tag k (0..7): 2 bits each in reserved_header, LSB
 * first (docs/plc_mqtt_design.md section 13, Q8). The App writes 0 today. */
static uint8_t tag_dec(const uint16_t *blk, unsigned k)
{
    return (uint8_t)((blk[TB_RESERVED] >> (2U * k)) & 0x3U);
}

/* Reset the publish state of block i after its running config changed (boot,
 * COMMIT, Flash import, clear): the COV thresholds are derived here ONCE so
 * the scan only compares integers, and the block publishes a baseline value
 * the next time it is active. */
static void rt_prepare(unsigned i)
{
    const uint16_t *blk = s_tel[i];
    block_rt_t     *rt  = &s_rt[i];
    uint32_t        bits = reg32(blk, TB_DEADBAND_HI);
    float           deadband;
    memcpy(&deadband, &bits, sizeof(deadband));

    memset(rt, 0, sizeof(*rt));
#if SPLC_REAL_ENABLE
    /* NaN, Inf and negative are rejected at COMMIT; FLT_MAX = "never" if one slipped through. */
    rt->cov_real = ((deadband >= 0.0f) && (deadband <= 3.4028234e38f)) ? deadband : 3.4028234e38f;
#endif
    for (unsigned k = 0; k < SPLC_MQTT_MAX_TAGS_PER_BLOCK; k++) {
        uint32_t thr;
        if (!splc_mqtt_fmt_deadband_threshold(deadband, tag_dec(blk, k), &thr)) {
            thr = UINT32_MAX;               /* unreachable for an enabled block: COMMIT rejects it */
        }
        rt->cov_thr[k] = thr;
    }
    rt->need_first = true;
}

static void rt_prepare_all(void)
{
    for (unsigned i = 0; i < BLOCKS; i++) {
        rt_prepare(i);
    }
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
    s_rr             = 0U;
    s_was_ready      = false;
    s_format_errors  = 0U;
    s_format_warned  = false;
    rt_prepare_all();                       /* s_publish / s_publish_ctx are kept on purpose */
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
    log_info(TAG, "telemetry write: offset=%u quantity=%u", offset, quantity);
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
    log_info(TAG, "broker write: offset=%u quantity=%u", offset, quantity);
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
    if ((brk[BK_CONTROL] & SPLC_BROKER_CTRL_USE_TLS_BIT) != 0U && !plc_mqtt_tls_ready()) {
        log_warn(TAG, "COMMIT: broker use_tls=1 but this build has no TLS (SPLC_MQTT_TLS_SUPPORTED=0 and no host TLS functions registered)");
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
    /* Diagnostic: what the COMMIT is about to apply (draft_mask bit 4 = broker, bits 0..3 = blocks). */
    log_info(TAG, "COMMIT: MQTT draft_mask=0x%02X broker_draft: enabled=%d host_empty=%d",
             (unsigned)s_draft_mask, (int)broker_enabled(s_brk_draft), (int)broker_host_empty(s_brk_draft));
    for (unsigned i = 0; i < BLOCKS; i++) {
        if ((s_draft_mask & (1U << i)) != 0U) {
            memcpy(s_tel[i], s_tel_draft[i], sizeof(s_tel[i]));
            rt_prepare(i);
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
    rt_prepare_all();
    broker_apply(zero_brk);
    s_draft_mask = 0U;
}

/* ------------------------------------------------------------------ Flash image (step E.3) */

#define IMG_TEL_BYTES    (BLOCKS * BLOCK_REGS * 2U)
#define IMG_BROKER_REGS  PLC_MQTT_BROKER_REGS     /* 96: 84 config + 12 reserved zero */
_Static_assert(IMG_TEL_BYTES + IMG_BROKER_REGS * 2U == PLC_MQTT_FLASH_SIZE,
               "PLC_MQTT_FLASH_SIZE must equal telemetry + broker register images");

static void put_regs_be(uint8_t *out, const uint16_t *regs, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        out[2U * i]      = (uint8_t)(regs[i] >> 8);
        out[2U * i + 1U] = (uint8_t)(regs[i] & 0xFFU);
    }
}

static void get_regs_be(const uint8_t *in, uint16_t *regs, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        regs[i] = (uint16_t)(((uint16_t)in[2U * i] << 8) | in[2U * i + 1U]);
    }
}

void plc_mqtt_export(uint8_t *out)
{
    put_regs_be(out, &s_tel[0][0], BLOCKS * BLOCK_REGS);
    memset(out + IMG_TEL_BYTES, 0, IMG_BROKER_REGS * 2U);
    put_regs_be(out + IMG_TEL_BYTES, s_brk, PLC_MQTT_BROKER_CFG_REGS);
}

bool plc_mqtt_import(const uint8_t *in)
{
    /* Decode into the DRAFT arrays (free scratch space), validate everything,
     * and only then touch the running config. Every block is checked, also
     * the ones the image holds as disabled (block_valid() returns early). */
    get_regs_be(in, &s_tel_draft[0][0], BLOCKS * BLOCK_REGS);
    get_regs_be(in + IMG_TEL_BYTES, s_brk_draft, PLC_MQTT_BROKER_CFG_REGS);

    bool ok = true;
    for (unsigned i = 0; i < BLOCKS && ok; i++) {
        ok = block_valid(s_tel_draft[i], i);
    }
    ok = ok && broker_valid(s_brk_draft);
    /* The 12 registers after the config are reserved and always written as 0. */
    for (size_t i = 0; ok && i < (IMG_BROKER_REGS - PLC_MQTT_BROKER_CFG_REGS) * 2U; i++) {
        ok = (in[IMG_TEL_BYTES + PLC_MQTT_BROKER_CFG_REGS * 2U + i] == 0U);
    }

    if (!ok) {
        s_draft_mask = 0U;
        log_warn(TAG, "Flash MQTT image failed validation -- MQTT config left as it was");
        return false;
    }

    memcpy(s_tel, s_tel_draft, sizeof(s_tel));
    rt_prepare_all();
    broker_apply(s_brk_draft);
    s_draft_mask = 0U;
    return true;
}

/* ------------------------------------------------------------------ publish scan (step E.4) */

bool splc_mqtt_register(splc_mqtt_publish_fn fn, void *ctx)
{
    if (fn == NULL) {
        return false;
    }
    s_publish     = fn;
    s_publish_ctx = ctx;
    return true;
}

uint32_t splc_mqtt_format_errors(void)
{
    return s_format_errors;
}

static bool broker_ready(void)
{
    return (s_status.flags & SPLC_MQTT_STATUS_CONNECTED) != 0U &&
           s_status.generation == s_generation;
}

static bool block_active(const uint16_t *blk)
{
    if ((blk[TB_CONTROL] & SPLC_MQTT_CTRL_ENABLED_BIT) == 0U) {
        return false;
    }
    const uint16_t en = blk[TB_EN_TAG];
    return (en == 0xFFFFU) || (en < MAX_TAGS && tag_read(en) != 0);
}

#if SPLC_REAL_ENABLE
static bool tag_is_real(const uint16_t *blk, unsigned k)
{
    return plc_tagtype_get(blk[TB_TAGS + k]) == PLC_TAGTYPE_REAL;
}

static float bits_to_float(int32_t v)
{
    float f;
    memcpy(&f, &v, sizeof(f));
    return f;
}

/* true when a bound REAL tag holds NaN or +-Inf: such a block is not published
 * (docs/handoff.md F.3 assumption), it waits for the next finite value. */
static bool block_has_nonfinite(const uint16_t *blk, const int32_t *cur, unsigned count)
{
    for (unsigned k = 0; k < count; k++) {
        if (tag_is_real(blk, k) && !splc_mqtt_fmt_real_is_finite((uint32_t)cur[k])) {
            return true;
        }
    }
    return false;
}
#endif

/* Should block i publish now? `cur` holds the current values of its bound tags. */
static bool block_due(unsigned i, uint32_t now_ms, const int32_t *cur)
{
    const uint16_t *blk = s_tel[i];
    const block_rt_t *rt = &s_rt[i];

    if (rt->need_first) {
        return true;
    }
    const unsigned mode  = (blk[TB_CONTROL] & SPLC_MQTT_CTRL_MODE_MASK) >> SPLC_MQTT_CTRL_MODE_SHIFT;
    const unsigned count = blk[TB_TAG_COUNT];

    switch (mode) {
        case SPLC_MQTT_MODE_ON_CHANGE:
            for (unsigned k = 0; k < count; k++) {
                if (cur[k] != rt->last_val[k]) {
                    return true;
                }
            }
            return false;
        case SPLC_MQTT_MODE_COV:
            for (unsigned k = 0; k < count; k++) {
#if SPLC_REAL_ENABLE
                if (tag_is_real(blk, k)) {      /* |cur - last| in engineering units, against the deadband */
                    if (cur[k] != rt->last_val[k]) {
                        float df = bits_to_float(cur[k]) - bits_to_float(rt->last_val[k]);
                        if (df < 0.0f) {
                            df = -df;
                        }
                        if (df >= rt->cov_real) {
                            return true;
                        }
                    }
                    continue;
                }
#endif
                int64_t d = (int64_t)cur[k] - (int64_t)rt->last_val[k];
                if (d < 0) {
                    d = -d;
                }
                if (d >= (int64_t)rt->cov_thr[k]) {
                    return true;
                }
            }
            return false;
        case SPLC_MQTT_MODE_PERIODIC:
            return (uint32_t)(now_ms - rt->last_pub_ms) >= reg32(blk, TB_INTERVAL_HI);
        default:
            return false;
    }
}

/* Render block i's payload into s_payload. Returns its length, or -1. */
static int block_render(unsigned i, const int32_t *cur)
{
    const uint16_t *blk = s_tel[i];
    char            device[SPLC_MQTT_BROKER_CRED_MAX_CHARS + 1U];
    splc_mqtt_fmt_in_t in;
    uint32_t           epoch = 0U;

    regs_to_str(&s_brk[BK_CLIENT_ID], SPLC_MQTT_BROKER_CRED_MAX_CHARS, device);
    in.device = (device[0] != '\0') ? device : s_default_device;
    in.count  = (uint8_t)blk[TB_TAG_COUNT];
    for (unsigned k = 0; k < SPLC_MQTT_FMT_MAX_TAGS; k++) {
        in.tag[k] = blk[TB_TAGS + k];
        in.val[k] = (k < in.count) ? cur[k] : 0;
        in.dec[k] = tag_dec(blk, k);
#if SPLC_REAL_ENABLE
        in.kind[k] = SPLC_MQTT_FMT_KIND_INT;
        if (k < in.count) {
            const plc_tagtype_t ty = plc_tagtype_get(blk[TB_TAGS + k]);
            if (ty == PLC_TAGTYPE_REAL) {
                in.kind[k] = SPLC_MQTT_FMT_KIND_REAL;
            } else if (ty == PLC_TAGTYPE_BOOL) {
                in.kind[k] = SPLC_MQTT_FMT_KIND_BOOL;
            }
        }
#endif
    }
    if (!plc_rtc_get_epoch(&epoch)) {
        epoch = 0U;                         /* no clock yet: {ts} = 0, never a made-up time */
    }
    in.ts = epoch;

    switch ((blk[TB_CONTROL] & SPLC_MQTT_CTRL_PAYLOAD_MASK) >> SPLC_MQTT_CTRL_PAYLOAD_SHIFT) {
        case SPLC_MQTT_PAYLOAD_COMPACT_JSON:
            return splc_mqtt_fmt_compact(s_payload, sizeof(s_payload), &in);
        case SPLC_MQTT_PAYLOAD_RAW_VALUE:
            return splc_mqtt_fmt_raw(s_payload, sizeof(s_payload), &in);
        case SPLC_MQTT_PAYLOAD_CUSTOM: {
            char tmpl[TB_TMPL_CHARS + 1U];
            regs_to_str(&blk[TB_TEMPLATE], TB_TMPL_CHARS, tmpl);
            return splc_mqtt_fmt_custom(s_payload, sizeof(s_payload), tmpl, TB_TMPL_CHARS, &in);
        }
        default:
            return -1;
    }
}

void plc_mqtt_scan(uint32_t now_ms)
{
    if (s_publish == NULL) {
        return;
    }

    const bool ready = broker_ready();
    if (ready && !s_was_ready) {
        /* Broker (re)connected, or just switched to the new config: every
         * block gives the cloud a fresh baseline value. */
        for (unsigned i = 0; i < BLOCKS; i++) {
            s_rt[i].need_first = true;
        }
    }
    s_was_ready = ready;
    if (!ready) {
        return;                             /* nothing is queued while not ready */
    }

    for (unsigned n = 0; n < BLOCKS; n++) {
        const unsigned   i   = (s_rr + n) % BLOCKS;
        const uint16_t  *blk = s_tel[i];
        block_rt_t      *rt  = &s_rt[i];

        const bool active = block_active(blk);
        if (!active) {
            rt->was_active = false;
            continue;
        }
        if (!rt->was_active) {              /* enable tag went 0 -> non-zero (or block just enabled) */
            rt->was_active = true;
            rt->need_first = true;
        }
        if (rt->retry_wait) {
            if ((uint32_t)(now_ms - rt->retry_at_ms) >= 0x80000000U) {
                continue;                   /* retry time still in the future */
            }
            rt->retry_wait = false;
        }

        int32_t cur[SPLC_MQTT_MAX_TAGS_PER_BLOCK];
        const unsigned count = blk[TB_TAG_COUNT];
        for (unsigned k = 0; k < count && k < SPLC_MQTT_MAX_TAGS_PER_BLOCK; k++) {
            cur[k] = tag_read(blk[TB_TAGS + k]);
        }
#if SPLC_REAL_ENABLE
        if (block_has_nonfinite(blk, cur, count)) {
            if (!s_nonfinite_warned) {
                s_nonfinite_warned = true;
                log_warn(TAG, "block %u: a REAL tag is NaN/Inf; not published until it is finite", i);
            }
            continue;
        }
#endif
        if (!block_due(i, now_ms, cur)) {
            continue;
        }

        s_rr = (i + 1U) % BLOCKS;           /* one publish attempt per scan, then the next block goes first */

        const int len = block_render(i, cur);
        if (len < 0) {
            s_format_errors++;
            if (!s_format_warned) {
                s_format_warned = true;
                log_warn(TAG, "block %u: payload could not be rendered; not published", i);
            }
            rt->retry_wait  = true;
            rt->retry_at_ms = (uint32_t)(now_ms + SPLC_MQTT_RETRY_MS);
            return;
        }

        char topic[TB_TOPIC_CHARS + 1U];
        regs_to_str(&blk[TB_TOPIC], TB_TOPIC_CHARS, topic);
        const uint8_t qos = (uint8_t)((blk[TB_CONTROL] & SPLC_MQTT_CTRL_QOS_MASK) >> SPLC_MQTT_CTRL_QOS_SHIFT);
        const bool retain = (blk[TB_CONTROL] & SPLC_MQTT_CTRL_RETAIN_BIT) != 0U;

        if (s_publish(topic, s_payload, (size_t)len, qos, retain, s_publish_ctx) == 0) {
            for (unsigned k = 0; k < count && k < SPLC_MQTT_MAX_TAGS_PER_BLOCK; k++) {
                rt->last_val[k] = cur[k];
            }
            rt->last_pub_ms = now_ms;
            rt->need_first  = false;
            s_tx_msgs++;
            s_tx_bytes += (uint32_t)len;
        } else {
            rt->retry_wait  = true;         /* nothing recorded as published */
            rt->retry_at_ms = (uint32_t)(now_ms + SPLC_MQTT_RETRY_MS);
        }
        return;
    }
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