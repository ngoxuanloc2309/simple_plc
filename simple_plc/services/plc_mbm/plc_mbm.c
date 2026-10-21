/*
 * plc_mbm.c - Layer 3. See plc_mbm.h.
 */

#include "plc_mbm.h"

#if SPLC_MBM_ENABLE

#include <string.h>

#include "plc_tag.h"
#include "splc_log.h"
#if SPLC_REAL_ENABLE
#include "plc_tagtype.h"
#endif

static const char *TAG = "PLC_MBM";

/* --- block field offsets (register index inside a block) ----------------- */
#define F_FLAGS        0U
#define F_FC_SLAVE     1U
#define F_START        2U
#define F_COUNT        3U
#define F_INTERVAL     4U   /* 2 regs */
#define F_TIMEOUT      6U   /* 2 regs */
#define F_VALID_TAG    8U
#define F_ERROR_TAG    9U
#define F_ERRCODE_TAG  10U
#define F_PIN_COUNT    11U
#define F_READ_MODE    12U
#define F_TRIG_TAG     13U
#define F_PINS         16U
#define PIN_REGS       12U
#define P_TAG          0U
#define P_OFFSET       1U
#define P_TYPE         2U
#define P_ENDIAN       3U
#define P_MULT         4U   /* 2 regs */

#define TAG_NONE_IDX   0xFFFFU

enum { DT_BOOL = 1, DT_INT16, DT_UINT16, DT_INT32, DT_UINT32, DT_REAL };
enum { EN_ABCD = 0, EN_CDAB, EN_BADC, EN_DCBA };
enum { ERR_NONE = 0, ERR_TIMEOUT = 1, ERR_CRC = 2, ERR_EXCEPTION = 3, ERR_MALFORMED = 4 };

#define MIN_INTERVAL_MS   PLC_SCAN_INTERVAL_MS
#define MIN_TIMEOUT_MS    10U
#define MAX_TIMEOUT_MS    60000U
#define CONFIGURE_RETRY_MS 1000U

typedef struct {
    bool     started;        /* cyclic: first poll already sent */
    bool     pending;        /* on-trigger: rising edge seen, poll not sent yet */
    bool     prev_trig;
    bool     valid_clear;    /* write valid tag back to 0 at the next scan */
    uint32_t last_start_ms;
} block_rt_t;

static uint16_t   s_run[PLC_MBM_BLOCKS][PLC_MBM_BLOCK_REGS];    /* RUNNING */
static uint16_t   s_draft[PLC_MBM_BLOCKS][PLC_MBM_BLOCK_REGS];  /* valid only where the mask bit is set */
static uint8_t    s_draft_mask;
static block_rt_t s_rt[PLC_MBM_BLOCKS];

static modbus_rtu_master_t   s_master;
static bool                  s_have_port;
static plc_mbm_configure_fn  s_configure;
static plc_mbm_serial_t      s_serial;
static bool                  s_serial_dirty;
static uint32_t              s_configure_next_ms;
static int8_t                s_active = -1;     /* block whose request is in flight */
static bool                  s_active_stale;    /* its block was re-committed: drop the answer */
static uint8_t               s_rr;              /* round-robin start */

/* ------------------------------------------------------------------------ */

static uint32_t get_u32(const uint16_t *r) { return ((uint32_t)r[0] << 16) | r[1]; }

static bool blk_enabled(const uint16_t *b) { return (b[F_FLAGS] & 0x0001U) != 0U; }
static uint8_t blk_fc(const uint16_t *b)    { return (uint8_t)(b[F_FC_SLAVE] >> 8); }
static uint8_t blk_slave(const uint16_t *b) { return (uint8_t)(b[F_FC_SLAVE] & 0xFFU); }
static bool fc_is_bit(uint8_t fc)           { return fc == 1U || fc == 2U; }

static float bits_to_float(uint32_t u) { float f; memcpy(&f, &u, sizeof(f)); return f; }
static uint32_t float_to_bits(float f) { uint32_t u; memcpy(&u, &f, sizeof(u)); return u; }

/* Round to nearest, saturating. No libm. */
static int32_t f2i(float x)
{
    if (x >= 2147483520.0f)  { return INT32_MAX; }
    if (x <= -2147483648.0f) { return INT32_MIN; }
    return (int32_t)((x >= 0.0f) ? (x + 0.5f) : (x - 0.5f));
}

static bool tag_writable(uint16_t idx)
{
    if (idx >= MAX_TAGS) { return false; }
    const SPLC_TagKind k = tag_get_kind(idx);
    return k == TAG_VFLAG || k == TAG_VREG || k == TAG_VREG_RETAIN ||
           k == TAG_MB_COIL || k == TAG_MB_HOLDING;
}

static bool opt_tag_ok(uint16_t idx) { return idx == TAG_NONE_IDX || tag_writable(idx); }

static uint16_t pin_width(uint16_t dt)
{
    return (dt == DT_INT32 || dt == DT_UINT32 || dt == DT_REAL) ? 2U : 1U;
}

/* COMMIT / import rules for ONE block. A disabled block is not checked: the
 * App writes zeros into the blocks it does not use. */
static bool block_valid(const uint16_t *b, unsigned idx)
{
    if (!blk_enabled(b)) {
        return true;
    }
    const uint8_t fc = blk_fc(b);
    const uint8_t sl = blk_slave(b);
    const uint16_t count = b[F_COUNT];
    const uint32_t interval = get_u32(&b[F_INTERVAL]);
    const uint32_t timeout  = get_u32(&b[F_TIMEOUT]);

    if (sl < 1U || sl > 247U)                        { log_warn(TAG, "block %u: slave %u", idx, sl); return false; }
    if (fc < 1U || fc > 4U)                          { log_warn(TAG, "block %u: function code %u", idx, fc); return false; }
    if (count < 1U || count > MODBUS_MASTER_MAX_REGS) { log_warn(TAG, "block %u: count %u", idx, count); return false; }
    if (timeout < MIN_TIMEOUT_MS || timeout > MAX_TIMEOUT_MS) { log_warn(TAG, "block %u: timeout %lu", idx, (unsigned long)timeout); return false; }
    if (b[F_READ_MODE] > 1U)                         { log_warn(TAG, "block %u: read mode %u", idx, b[F_READ_MODE]); return false; }
    if (b[F_READ_MODE] == 0U) {
        if (interval < MIN_INTERVAL_MS || interval > 3600000UL) { log_warn(TAG, "block %u: interval %lu", idx, (unsigned long)interval); return false; }
    } else if (b[F_TRIG_TAG] >= MAX_TAGS) {          /* a trigger tag is only read: any kind, but it must exist */
        log_warn(TAG, "block %u: trigger tag %u", idx, b[F_TRIG_TAG]); return false;
    }
    if (!opt_tag_ok(b[F_VALID_TAG]) || !opt_tag_ok(b[F_ERROR_TAG]) || !opt_tag_ok(b[F_ERRCODE_TAG])) {
        log_warn(TAG, "block %u: status tag not writable", idx); return false;
    }
    if (b[F_PIN_COUNT] > PLC_MBM_MAX_PINS)           { log_warn(TAG, "block %u: pin count %u", idx, b[F_PIN_COUNT]); return false; }

    for (uint16_t i = 0U; i < b[F_PIN_COUNT]; i++) {
        const uint16_t *p = &b[F_PINS + i * PIN_REGS];
        const uint16_t dt = p[P_TYPE];
        if (!opt_tag_ok(p[P_TAG]))      { log_warn(TAG, "block %u pin %u: target tag %u not writable", idx, i, p[P_TAG]); return false; }
        if (dt < DT_BOOL || dt > DT_REAL) { log_warn(TAG, "block %u pin %u: data type %u", idx, i, dt); return false; }
        if (p[P_ENDIAN] > EN_DCBA)      { log_warn(TAG, "block %u pin %u: endianness %u", idx, i, p[P_ENDIAN]); return false; }
        const uint16_t w = pin_width(dt);
        if (fc_is_bit(fc) && w != 1U)   { log_warn(TAG, "block %u pin %u: 32-bit type on a bit function code", idx, i); return false; }
        if ((uint32_t)p[P_OFFSET] + w > count) { log_warn(TAG, "block %u pin %u: offset %u outside count", idx, i, p[P_OFFSET]); return false; }
    }
    return true;
}

static void rt_reset(unsigned i)
{
    memset(&s_rt[i], 0, sizeof(s_rt[i]));
    if (s_active == (int8_t)i) {
        s_active_stale = true;       /* the answer on the wire belongs to the old config */
    }
}

static void write_status(uint16_t tag, int32_t v)
{
    if (tag != TAG_NONE_IDX) { tag_write(tag, v); }
}

/* ------------------------------------------------------------------------ */

void plc_mbm_init(void)
{
    plc_mbm_clear_all();
    s_serial.baud      = SPLC_MBM_SERIAL_BAUD;
    s_serial.parity    = (uint8_t)SPLC_MBM_SERIAL_PARITY;
    s_serial.stop_bits = (uint8_t)SPLC_MBM_SERIAL_STOP_BITS;
    s_serial_dirty     = true;
    s_configure_next_ms = 0U;
}

void plc_mbm_clear_all(void)
{
    memset(s_run, 0, sizeof(s_run));
    memset(s_draft, 0, sizeof(s_draft));
    s_draft_mask = 0U;
    for (unsigned i = 0U; i < PLC_MBM_BLOCKS; i++) { rt_reset(i); }
}

void splc_mbm_set_rs485_port(const modbus_serial_port_t *port, plc_mbm_configure_fn configure)
{
    s_have_port = (port != NULL) && modbus_rtu_master_init(&s_master, port);
    s_configure = configure;
    s_serial_dirty = true;
    s_configure_next_ms = 0U;
    s_active = -1;
    log_info(TAG, "RS485 port %s, configure callback %s", s_have_port ? "registered" : "NOT usable",
             (configure != NULL) ? "present" : "absent (line speed = port baudrate)");
}

bool plc_mbm_set_serial(const plc_mbm_serial_t *cfg)
{
    if (cfg == NULL || cfg->baud < 1200U || cfg->baud > 921600U || cfg->parity > 2U ||
        (cfg->stop_bits != 1U && cfg->stop_bits != 2U)) {
        return false;
    }
    if (cfg->baud != s_serial.baud || cfg->parity != s_serial.parity || cfg->stop_bits != s_serial.stop_bits) {
        s_serial = *cfg;
        s_serial_dirty = true;
        s_configure_next_ms = 0U;
    }
    return true;
}

void plc_mbm_get_serial(plc_mbm_serial_t *out) { if (out != NULL) { *out = s_serial; } }

void plc_mbm_read(uint16_t offset, uint16_t quantity, uint16_t *registers_out)
{
    for (uint16_t i = 0U; i < quantity; i++) {
        const uint16_t o = (uint16_t)(offset + i);
        registers_out[i] = (o < PLC_MBM_REGS) ? s_run[o / PLC_MBM_BLOCK_REGS][o % PLC_MBM_BLOCK_REGS] : 0U;
    }
}

plc_mbm_write_result_t plc_mbm_write(uint16_t offset, uint16_t quantity, const uint16_t *registers)
{
    if ((uint32_t)offset + quantity > PLC_MBM_REGS) {
        return PLC_MBM_WRITE_BAD_ADDRESS;
    }
    for (uint16_t i = 0U; i < quantity; i++) {
        const uint16_t o = (uint16_t)(offset + i);
        const unsigned blk = o / PLC_MBM_BLOCK_REGS;
        if ((s_draft_mask & (1U << blk)) == 0U) {
            memcpy(s_draft[blk], s_run[blk], sizeof(s_draft[blk]));   /* a partial write keeps the rest */
            s_draft_mask |= (uint8_t)(1U << blk);
        }
        s_draft[blk][o % PLC_MBM_BLOCK_REGS] = registers[i];
    }
    return PLC_MBM_WRITE_OK;
}

bool plc_mbm_validate_draft(void)
{
    for (unsigned i = 0U; i < PLC_MBM_BLOCKS; i++) {
        if ((s_draft_mask & (1U << i)) != 0U && !block_valid(s_draft[i], i)) {
            return false;
        }
    }
    return true;
}

void plc_mbm_commit_draft(void)
{
    unsigned enabled = 0U;
    for (unsigned i = 0U; i < PLC_MBM_BLOCKS; i++) {
        if ((s_draft_mask & (1U << i)) != 0U) {
            memcpy(s_run[i], s_draft[i], sizeof(s_run[i]));
            rt_reset(i);
        }
        if (blk_enabled(s_run[i])) { enabled++; }
    }
    if (s_draft_mask != 0U) {
        log_info(TAG, "COMMIT: draft_mask=0x%02X, %u block(s) enabled", (unsigned)s_draft_mask, enabled);
    }
    s_draft_mask = 0U;
}

void plc_mbm_discard_draft(void) { s_draft_mask = 0U; }

void plc_mbm_export(uint8_t *out)
{
    for (unsigned i = 0U; i < PLC_MBM_REGS; i++) {
        const uint16_t v = s_run[i / PLC_MBM_BLOCK_REGS][i % PLC_MBM_BLOCK_REGS];
        out[2U * i]      = (uint8_t)(v >> 8);
        out[2U * i + 1U] = (uint8_t)(v & 0xFFU);
    }
}

static void unpack_block(const uint8_t *in, unsigned blk, uint16_t out[PLC_MBM_BLOCK_REGS])
{
    for (unsigned i = 0U; i < PLC_MBM_BLOCK_REGS; i++) {
        const unsigned o = blk * PLC_MBM_BLOCK_REGS + i;
        out[i] = (uint16_t)(((uint16_t)in[2U * o] << 8) | in[2U * o + 1U]);
    }
}

bool plc_mbm_import(const uint8_t *in)
{
    uint16_t blk[PLC_MBM_BLOCK_REGS];   /* 128 B on the stack: no 512 B static copy of the image */
    for (unsigned i = 0U; i < PLC_MBM_BLOCKS; i++) {
        unpack_block(in, i, blk);
        if (!block_valid(blk, i)) {
            return false;               /* nothing applied yet */
        }
    }
    for (unsigned i = 0U; i < PLC_MBM_BLOCKS; i++) {
        unpack_block(in, i, s_run[i]);
        rt_reset(i);
    }
    s_draft_mask = 0U;
    return true;
}

/* ------------------------------------------------------------------------ */
/* Decoding                                                                 */

static uint16_t swap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

static uint32_t word32(uint16_t r0, uint16_t r1, uint16_t en)
{
    const uint32_t b0 = r0 >> 8, b1 = r0 & 0xFFU, b2 = r1 >> 8, b3 = r1 & 0xFFU;
    switch (en) {
    case EN_CDAB: return (b2 << 24) | (b3 << 16) | (b0 << 8) | b1;
    case EN_BADC: return (b1 << 24) | (b0 << 16) | (b3 << 8) | b2;
    case EN_DCBA: return (b3 << 24) | (b2 << 16) | (b1 << 8) | b0;
    default:      return (b0 << 24) | (b1 << 16) | (b2 << 8) | b3;   /* ABCD */
    }
}

/* Decode one pin of an answer into the value to store in the tag. false = the
 * pin cannot be decoded (NaN/Inf, outside the answer): leave the tag alone. */
static bool decode_pin(const uint16_t *pin, const uint16_t *r, uint16_t count, int32_t *out)
{
    const uint16_t off = pin[P_OFFSET], dt = pin[P_TYPE], en = pin[P_ENDIAN];
    if ((uint32_t)off + pin_width(dt) > count) { return false; }

    float mult = bits_to_float(get_u32(&pin[P_MULT]));
    const bool unity = (mult == 0.0f) || (mult == 1.0f);
    if (mult == 0.0f) { mult = 1.0f; }                    /* the App treats 0 as "no scaling" */

#if SPLC_REAL_ENABLE
    const bool target_real = plc_tagtype_is_real(pin[P_TAG]);
#else
    const bool target_real = false;
#endif

    switch (dt) {
    case DT_BOOL:
        *out = (r[off] != 0U) ? 1 : 0;
        return true;

    case DT_INT16:
    case DT_UINT16: {
        uint16_t raw = r[off];
        if (en == EN_BADC || en == EN_DCBA) { raw = swap16(raw); }
        const int32_t iv = (dt == DT_INT16) ? (int32_t)(int16_t)raw : (int32_t)raw;
        if (unity)            { *out = iv; }
        else if (target_real) { *out = (int32_t)float_to_bits((float)iv * mult); }
        else                  { *out = f2i((float)iv * mult); }
        return true;
    }

    case DT_INT32:
    case DT_UINT32: {
        const uint32_t raw = word32(r[off], r[off + 1U], en);
        if (unity) {
            *out = (int32_t)raw;
        } else {
            const float fv = (dt == DT_INT32) ? (float)(int32_t)raw : (float)raw;
            *out = target_real ? (int32_t)float_to_bits(fv * mult) : f2i(fv * mult);
        }
        return true;
    }

    default: { /* DT_REAL */
        const float fv = bits_to_float(word32(r[off], r[off + 1U], en));
        const uint32_t e = (float_to_bits(fv) >> 23) & 0xFFU;
        if (e == 0xFFU) { return false; }                 /* NaN / Inf */
        const float v = fv * mult;
#if SPLC_REAL_ENABLE
        *out = (int32_t)float_to_bits(v);
#else
        *out = f2i(v);
#endif
        return true;
    }
    }
}

static void finish_ok(unsigned blk)
{
    const uint16_t *b = s_run[blk];
    const uint16_t *r = modbus_rtu_master_regs(&s_master);
    for (uint16_t i = 0U; i < b[F_PIN_COUNT]; i++) {
        const uint16_t *pin = &b[F_PINS + i * PIN_REGS];
        int32_t v;
        if (pin[P_TAG] != TAG_NONE_IDX && decode_pin(pin, r, b[F_COUNT], &v)) {
            tag_write(pin[P_TAG], v);
        }
    }
    write_status(b[F_ERROR_TAG], 0);
    write_status(b[F_ERRCODE_TAG], ERR_NONE);
    write_status(b[F_VALID_TAG], 1);
    s_rt[blk].valid_clear = (b[F_VALID_TAG] != TAG_NONE_IDX);
}

static void finish_err(unsigned blk)
{
    const uint16_t *b = s_run[blk];
    int32_t code;
    switch (modbus_rtu_master_error(&s_master)) {
    case MODBUS_MASTER_ERR_CRC:       code = ERR_CRC;       break;
    case MODBUS_MASTER_ERR_EXCEPTION: code = ERR_EXCEPTION; break;
    case MODBUS_MASTER_ERR_FRAME:     code = ERR_MALFORMED; break;
    default:                          code = ERR_TIMEOUT;   break;   /* timeout, failed TX */
    }
    write_status(b[F_VALID_TAG], 0);
    write_status(b[F_ERROR_TAG], 1);
    write_status(b[F_ERRCODE_TAG], code);
    s_rt[blk].valid_clear = false;
}

/* ------------------------------------------------------------------------ */

static void apply_serial(uint32_t now_ms)
{
    if (!s_serial_dirty || (int32_t)(now_ms - s_configure_next_ms) < 0) {
        return;
    }
    if (s_configure == NULL) {
        s_serial_dirty = false;       /* nothing to apply: the port keeps its own line speed */
        log_info(TAG, "no configure callback: line settings stay as the host set them (baud %lu)",
                 (unsigned long)s_master.port.baudrate);
        return;
    }
    if (s_configure(s_master.port.ctx, &s_serial) == 0) {
        s_master.port.baudrate = s_serial.baud;   /* only feeds the inter-frame gap */
        s_serial_dirty = false;
        log_info(TAG, "serial applied: %lu baud, parity %u, stop %u",
                 (unsigned long)s_serial.baud, (unsigned)s_serial.parity, (unsigned)s_serial.stop_bits);
    } else {
        s_configure_next_ms = now_ms + CONFIGURE_RETRY_MS;
        log_warn(TAG, "host could not apply %lu baud / parity %u / stop %u, retrying",
                 (unsigned long)s_serial.baud, (unsigned)s_serial.parity, (unsigned)s_serial.stop_bits);
    }
}

static bool block_due(unsigned i, uint32_t now_ms)
{
    const uint16_t *b = s_run[i];
    if (!blk_enabled(b)) { return false; }
    if (b[F_READ_MODE] == 1U) { return s_rt[i].pending; }
    return !s_rt[i].started ||
           (uint32_t)(now_ms - s_rt[i].last_start_ms) >= get_u32(&b[F_INTERVAL]);
}

void plc_mbm_scan(uint32_t now_ms)
{
    if (!s_have_port) {
        return;
    }
    apply_serial(now_ms);
    if (s_serial_dirty && s_configure != NULL) {
        return;                       /* do not poll with the wrong line settings */
    }

    /* The valid tag is a one-scan pulse. */
    for (unsigned i = 0U; i < PLC_MBM_BLOCKS; i++) {
        if (s_rt[i].valid_clear) {
            write_status(s_run[i][F_VALID_TAG], 0);
            s_rt[i].valid_clear = false;
        }
    }

    /* Rising edge of every on-trigger block's tag, even while the bus is busy. */
    for (unsigned i = 0U; i < PLC_MBM_BLOCKS; i++) {
        const uint16_t *b = s_run[i];
        if (blk_enabled(b) && b[F_READ_MODE] == 1U) {
            const bool now = tag_read(b[F_TRIG_TAG]) != 0;
            if (now && !s_rt[i].prev_trig) { s_rt[i].pending = true; }
            s_rt[i].prev_trig = now;
        }
    }

    if (s_active >= 0) {
        const modbus_master_state_t st = modbus_rtu_master_poll(&s_master);
        if (st == MODBUS_MASTER_BUSY) {
            return;
        }
        const unsigned blk = (unsigned)s_active;
        s_active = -1;
        if (!s_active_stale) {
            if (st == MODBUS_MASTER_DONE_OK)       { finish_ok(blk); }
            else if (st == MODBUS_MASTER_DONE_ERR) { finish_err(blk); }
        }
        s_active_stale = false;
    }

    for (unsigned n = 0U; n < PLC_MBM_BLOCKS; n++) {
        const unsigned i = (s_rr + n) % PLC_MBM_BLOCKS;
        if (!block_due(i, now_ms)) { continue; }
        const uint16_t *b = s_run[i];
        if (modbus_rtu_master_read_fc(&s_master, blk_slave(b), blk_fc(b), b[F_START], b[F_COUNT],
                                      get_u32(&b[F_TIMEOUT]))) {
            s_active = (int8_t)i;
            s_active_stale = false;
            s_rt[i].started = true;
            s_rt[i].pending = false;
            s_rt[i].last_start_ms = now_ms;
            s_rr = (uint8_t)((i + 1U) % PLC_MBM_BLOCKS);
        }
        /* false: the inter-frame gap has not elapsed (retry next scan) -- nothing consumed */
        break;
    }
}

#endif /* SPLC_MBM_ENABLE */