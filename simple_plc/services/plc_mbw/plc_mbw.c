/*
 * plc_mbw.c - Layer 3. See plc_mbw.h.
 */

#include "plc_mbw.h"

#if SPLC_MBW_ENABLE

#include <string.h>

#include "plc_mbm.h"
#include "plc_tag.h"
#include "splc_log.h"
#if SPLC_REAL_ENABLE
#include "plc_tagtype.h"
#endif

static const char *TAG = "PLC_MBW";

#define PORTS    ((unsigned)SPLC_MBM_PORT_COUNT)

/* --- block field offsets (register index inside a block) ----------------- */
#define F_FLAGS        0U
#define F_FC_SLAVE     1U
#define F_START        2U   /* Direct: start address;  Channel: base command address */
#define F_COUNT        3U   /* Direct: point count;    Channel: channel count */
#define F_PARAM_BASE   4U   /* 2 regs, Channel: base parameter address */
#define F_INTERVAL     6U   /* 2 regs */
#define F_TIMEOUT      8U   /* 2 regs */
#define F_TRIG_MODE    10U
#define F_TRIG_TAG     11U
#define F_DONE_TAG     12U
#define F_BUSY_TAG     13U
#define F_ERROR_TAG    14U
#define F_ERRCODE_TAG  15U
#define F_PAYLOAD      16U
#define F_CH_TAG       16U  /* Channel mode */
#define F_CMD_TAG      17U
#define F_PARAM_TAG    18U

#define PIN_REGS       12U
#define P_TAG          0U
#define P_OFFSET       1U
#define P_TYPE         2U
#define P_ENDIAN       3U
#define P_MULT         4U   /* 2 regs */

#define FLAG_ENABLED     0x0001U
#define FLAG_EXCLUSIVE   0x0002U
#define FLAG_MODE_SHIFT  2U
#define FLAG_MODE_MASK   0x03U
#define FLAG_PORT_SHIFT  4U
#define FLAG_PORT_MASK   0x0FU

#define TAG_NONE_IDX   0xFFFFU

#define MODE_DIRECT    0U
#define MODE_CHANNEL   1U

#define TRIG_EDGE      0U
#define TRIG_CHANGE    1U
#define TRIG_CYCLIC    2U

enum { DT_BOOL = 1, DT_INT16, DT_UINT16, DT_INT32, DT_UINT32, DT_REAL };
enum { EN_ABCD = 0, EN_CDAB, EN_BADC, EN_DCBA };
enum { ERR_NONE = 0, ERR_TIMEOUT = 1, ERR_CRC = 2, ERR_EXCEPTION = 3, ERR_BUS_BUSY = 4,
       ERR_CHANNEL = 5, ERR_BAD_VALUE = 6 };

#define MIN_INTERVAL_MS   PLC_SCAN_INTERVAL_MS
#define MIN_TIMEOUT_MS    10U
#define MAX_TIMEOUT_MS    60000U
#define MAX_INTERVAL_MS   3600000UL
#define BUS_WAIT_MS       2000U     /* a frame that cannot get its bus for this long fails with err_code 4 */

enum { ST_IDLE = 0, ST_SEND, ST_WAIT };
enum { PH_CLEAR = 0, PH_PARAM, PH_CMD };

typedef struct {
    uint8_t   state;
    uint8_t   phase;         /* ChannelIndexed: what the job writes next */
    uint8_t   port;          /* port whose master is in flight (ST_WAIT) */
    uint8_t   retries_left;
    uint8_t   step_ch;       /* channel (1-based) of the clear frame in flight */
    bool      started;       /* a job ran since COMMIT (rate limit of cyclic / on-change) */
    bool      pending;       /* rising edge seen, job not started yet */
    bool      prev_trig;
    bool      baselined;     /* on-change: `last` holds the values of the last job */
    bool      done_clear;    /* write the done tag back to 0 at the next scan */
    bool      in_retry;      /* the failed frame is being repeated */
    bool      abandon;       /* the block was re-committed while its frame was in flight: drop the answer */
    bool      warned_port;
    uint16_t  job_ch;
    uint16_t  job_cmd;
    uint32_t  job_param;
    uint32_t  lit;           /* ChannelIndexed: bit n-1 set = channel n was last written non-zero */
    uint32_t  last_start_ms;
    uint32_t  wait_since_ms;
    int32_t   last[PLC_MBW_MAX_PINS];
    modbus_rtu_master_t *m;
} blk_rt_t;

static uint16_t  s_run[PLC_MBW_BLOCKS][PLC_MBW_BLOCK_REGS];     /* RUNNING */
static uint16_t  s_draft[PLC_MBW_BLOCKS][PLC_MBW_BLOCK_REGS];   /* valid only where the mask bit is set */
static uint8_t   s_draft_mask;
static blk_rt_t  s_rt[PLC_MBW_BLOCKS];
static uint8_t   s_rr;

/* ------------------------------------------------------------------------ */

static uint32_t get_u32(const uint16_t *r)        { return ((uint32_t)r[0] << 16) | r[1]; }
static uint16_t swap16(uint16_t v)                { return (uint16_t)((v >> 8) | (v << 8)); }
static bool     blk_enabled(const uint16_t *b)    { return (b[F_FLAGS] & FLAG_ENABLED) != 0U; }
static bool     blk_exclusive(const uint16_t *b)  { return (b[F_FLAGS] & FLAG_EXCLUSIVE) != 0U; }
static unsigned blk_mode(const uint16_t *b)       { return (unsigned)((b[F_FLAGS] >> FLAG_MODE_SHIFT) & FLAG_MODE_MASK); }
static unsigned blk_port(const uint16_t *b)       { return (unsigned)((b[F_FLAGS] >> FLAG_PORT_SHIFT) & FLAG_PORT_MASK); }
static uint8_t  blk_fc(const uint16_t *b)         { return (uint8_t)(b[F_FC_SLAVE] >> 8); }
static uint8_t  blk_slave(const uint16_t *b)      { return (uint8_t)(b[F_FC_SLAVE] & 0xFFU); }

static float    bits_to_float(uint32_t u) { float f; memcpy(&f, &u, sizeof(f)); return f; }
static uint32_t float_to_bits(float f)    { uint32_t u; memcpy(&u, &f, sizeof(u)); return u; }
static bool     float_finite(float f)     { return ((float_to_bits(f) >> 23) & 0xFFU) != 0xFFU; }

/* Round to nearest, saturating. No libm. */
static int32_t f2i(float x)
{
    if (x >=  2147483520.0f) { return INT32_MAX; }
    if (x <= -2147483648.0f) { return INT32_MIN; }
    return (int32_t)((x >= 0.0f) ? (x + 0.5f) : (x - 0.5f));
}

static uint32_t f2u(float x)
{
    if (x <= 0.0f)          { return 0U; }
    if (x >= 4294967040.0f) { return 0xFFFFFFFFUL; }
    return (uint32_t)(x + 0.5f);
}

static bool tag_exists(uint16_t idx)   { return idx < MAX_TAGS && tag_get_kind(idx) != TAG_NONE; }
static bool opt_exists(uint16_t idx)   { return idx == TAG_NONE_IDX || tag_exists(idx); }

static bool tag_writable(uint16_t idx)
{
    if (idx >= MAX_TAGS) { return false; }
    const SPLC_TagKind k = tag_get_kind(idx);
    return k == TAG_VFLAG || k == TAG_VREG || k == TAG_VREG_RETAIN || k == TAG_MB_COIL || k == TAG_MB_HOLDING;
}

static bool opt_tag_ok(uint16_t idx)   { return idx == TAG_NONE_IDX || tag_writable(idx); }

static bool tag_is_real(uint16_t idx)
{
#if SPLC_REAL_ENABLE
    return plc_tagtype_is_real(idx);
#else
    (void)idx;
    return false;
#endif
}

static uint16_t pin_width(uint16_t dt) { return (dt == DT_INT32 || dt == DT_UINT32 || dt == DT_REAL) ? 2U : 1U; }

static void write_status(uint16_t tag, int32_t v)
{
    if (tag != TAG_NONE_IDX) { tag_write(tag, v); }
}

/* ------------------------------------------------------------------------ */
/* COMMIT / import rules for ONE block. A disabled block is not checked.    */

static bool direct_valid(const uint16_t *b, unsigned idx)
{
    const uint8_t  fc    = blk_fc(b);
    const uint16_t count = b[F_COUNT];

    if (fc != 5U && fc != 6U && fc != 15U && fc != 16U) {
        log_warn(TAG, "block %u: function code %u (5, 6, 15 or 16)", idx, fc);
        return false;
    }
    if (count < 1U || count > PLC_MBW_MAX_COUNT) {
        log_warn(TAG, "block %u: point count %u (1..%u)", idx, count, (unsigned)PLC_MBW_MAX_COUNT);
        return false;
    }
    if ((fc == 5U || fc == 6U) && count != 1U) {
        log_warn(TAG, "block %u: function code %u writes exactly one point", idx, fc);
        return false;
    }

    uint32_t used = 0U;
    unsigned pins = 0U;
    for (unsigned i = 0U; i < PLC_MBW_MAX_PINS; i++) {
        const uint16_t *p = &b[F_PAYLOAD + i * PIN_REGS];
        if (p[P_TAG] == TAG_NONE_IDX) { continue; }
        pins++;
        const uint16_t dt = p[P_TYPE];
        if (!tag_exists(p[P_TAG]))        { log_warn(TAG, "block %u pin %u: source tag %u does not exist", idx, i, p[P_TAG]); return false; }
        if (dt < DT_BOOL || dt > DT_REAL) { log_warn(TAG, "block %u pin %u: data type %u", idx, i, dt); return false; }
        if (p[P_ENDIAN] > EN_DCBA)        { log_warn(TAG, "block %u pin %u: endianness %u", idx, i, p[P_ENDIAN]); return false; }
        const uint16_t w = pin_width(dt);
        if ((fc == 5U || fc == 15U) && dt != DT_BOOL) { log_warn(TAG, "block %u pin %u: a coil function code takes BOOL pins only", idx, i); return false; }
        if (fc == 6U && w != 1U)          { log_warn(TAG, "block %u pin %u: FC06 writes one 16-bit register", idx, i); return false; }
        if ((uint32_t)p[P_OFFSET] + w > count) { log_warn(TAG, "block %u pin %u: offset %u outside the point count", idx, i, p[P_OFFSET]); return false; }
        const uint32_t m = (((1UL << w) - 1UL) << p[P_OFFSET]);
        if ((used & m) != 0U)             { log_warn(TAG, "block %u pin %u: overlaps another pin", idx, i); return false; }
        used |= m;
    }
    if (pins == 0U) { log_warn(TAG, "block %u: Direct mode without a pin", idx); return false; }
    return true;
}

static bool channel_valid(const uint16_t *b, unsigned idx)
{
    const uint16_t count = b[F_COUNT];
    const uint32_t pbase = get_u32(&b[F_PARAM_BASE]);

    if (count < 1U || count > PLC_MBW_MAX_CHANNELS) {
        log_warn(TAG, "block %u: channel count %u (1..%u)", idx, count, (unsigned)PLC_MBW_MAX_CHANNELS);
        return false;
    }
    if ((uint32_t)b[F_START] + count - 1U > 0xFFFFU) {
        log_warn(TAG, "block %u: command addresses run past 0xFFFF", idx);
        return false;
    }
    if (pbase + ((uint32_t)(count - 1U)) * 2U + 1U > 0xFFFFU) {
        log_warn(TAG, "block %u: parameter addresses run past 0xFFFF", idx);
        return false;
    }
    if (!opt_exists(b[F_CH_TAG]) || !opt_exists(b[F_CMD_TAG]) || !opt_exists(b[F_PARAM_TAG])) {
        log_warn(TAG, "block %u: a channel / command / parameter tag does not exist", idx);
        return false;
    }
    return true;
}

static bool block_valid(const uint16_t *b, unsigned idx)
{
    if (!blk_enabled(b)) { return true; }

    const uint32_t interval = get_u32(&b[F_INTERVAL]);
    const uint32_t timeout  = get_u32(&b[F_TIMEOUT]);
    const unsigned mode     = blk_mode(b);
    const uint16_t trig     = b[F_TRIG_MODE];

    if (blk_port(b) >= PORTS) { log_warn(TAG, "block %u: port %u, the device has %u", idx, blk_port(b) + 1U, PORTS); return false; }
    if (blk_slave(b) < 1U || blk_slave(b) > 247U) { log_warn(TAG, "block %u: slave %u", idx, blk_slave(b)); return false; }
    if (mode > MODE_CHANNEL)  { log_warn(TAG, "block %u: write mode %u", idx, mode); return false; }
    if (timeout < MIN_TIMEOUT_MS || timeout > MAX_TIMEOUT_MS) { log_warn(TAG, "block %u: timeout %lu", idx, (unsigned long)timeout); return false; }
    if (trig > TRIG_CYCLIC)   { log_warn(TAG, "block %u: trigger mode %u", idx, trig); return false; }
    if (trig == TRIG_EDGE && b[F_TRIG_TAG] >= MAX_TAGS) {   /* only read: any kind, but it must exist */
        log_warn(TAG, "block %u: trigger tag %u", idx, b[F_TRIG_TAG]);
        return false;
    }
    if (trig == TRIG_CYCLIC && (interval < MIN_INTERVAL_MS || interval > MAX_INTERVAL_MS)) {
        log_warn(TAG, "block %u: cyclic interval %lu", idx, (unsigned long)interval);
        return false;
    }
    if (trig == TRIG_CHANGE && interval > MAX_INTERVAL_MS) {
        log_warn(TAG, "block %u: interval %lu", idx, (unsigned long)interval);
        return false;
    }
    if (!opt_tag_ok(b[F_DONE_TAG]) || !opt_tag_ok(b[F_BUSY_TAG]) ||
        !opt_tag_ok(b[F_ERROR_TAG]) || !opt_tag_ok(b[F_ERRCODE_TAG])) {
        log_warn(TAG, "block %u: status tag not writable", idx);
        return false;
    }
    return (mode == MODE_DIRECT) ? direct_valid(b, idx) : channel_valid(b, idx);
}

/* ------------------------------------------------------------------------ */

static void quiesce(unsigned i)
{
    /* The block that is about to be replaced must not leave its busy tag at 1. */
    if (blk_enabled(s_run[i])) {
        write_status(s_run[i][F_BUSY_TAG], 0);
        write_status(s_run[i][F_DONE_TAG], 0);
    }
}

static void rt_reset(unsigned i)
{
    blk_rt_t *r = &s_rt[i];
    if (r->state == ST_WAIT) {                     /* a frame is on the bus: let it finish, drop its answer */
        modbus_rtu_master_t *m = r->m;
        const uint8_t port = r->port;
        memset(r, 0, sizeof(*r));
        r->state = ST_WAIT; r->m = m; r->port = port; r->abandon = true;
        return;
    }
    memset(r, 0, sizeof(*r));
}

void plc_mbw_init(void)
{
    memset(s_run, 0, sizeof(s_run));
    memset(s_draft, 0, sizeof(s_draft));
    s_draft_mask = 0U;
    memset(s_rt, 0, sizeof(s_rt));
    s_rr = 0U;
}

void plc_mbw_clear_all(void)
{
    for (unsigned i = 0U; i < PLC_MBW_BLOCKS; i++) { quiesce(i); }
    memset(s_run, 0, sizeof(s_run));
    memset(s_draft, 0, sizeof(s_draft));
    s_draft_mask = 0U;
    for (unsigned i = 0U; i < PLC_MBW_BLOCKS; i++) { rt_reset(i); }
}

void plc_mbw_read(uint16_t offset, uint16_t quantity, uint16_t *registers_out)
{
    for (uint16_t i = 0U; i < quantity; i++) {
        const uint16_t o = (uint16_t)(offset + i);
        registers_out[i] = (o < PLC_MBW_REGS) ? s_run[o / PLC_MBW_BLOCK_REGS][o % PLC_MBW_BLOCK_REGS] : 0U;
    }
}

plc_mbw_write_result_t plc_mbw_write(uint16_t offset, uint16_t quantity, const uint16_t *registers)
{
    if ((uint32_t)offset + quantity > PLC_MBW_REGS) { return PLC_MBW_WRITE_BAD_ADDRESS; }
    for (uint16_t i = 0U; i < quantity; i++) {
        const uint16_t o   = (uint16_t)(offset + i);
        const unsigned blk = o / PLC_MBW_BLOCK_REGS;
        if ((s_draft_mask & (1U << blk)) == 0U) {
            memcpy(s_draft[blk], s_run[blk], sizeof(s_draft[blk]));   /* a partial write keeps the rest */
            s_draft_mask |= (uint8_t)(1U << blk);
        }
        s_draft[blk][o % PLC_MBW_BLOCK_REGS] = registers[i];
    }
    return PLC_MBW_WRITE_OK;
}

bool plc_mbw_validate_draft(void)
{
    for (unsigned i = 0U; i < PLC_MBW_BLOCKS; i++) {
        if ((s_draft_mask & (1U << i)) != 0U && !block_valid(s_draft[i], i)) { return false; }
    }
    /* An enabled block (as it will be after this COMMIT) must not name an enabled Slave port,
     * judged on the port table as it will be after this COMMIT (plc_mbm). */
    for (unsigned i = 0U; i < PLC_MBW_BLOCKS; i++) {
        const uint16_t *b = ((s_draft_mask & (1U << i)) != 0U) ? s_draft[i] : s_run[i];
        if (blk_enabled(b) && plc_mbm_port_is_slave_after_commit(blk_port(b))) {
            log_warn(TAG, "MB_WRITE block %u uses port %u, which is a Modbus Slave", i, blk_port(b) + 1U);
            return false;
        }
    }
    return true;
}

void plc_mbw_commit_draft(void)
{
    unsigned enabled = 0U;
    for (unsigned i = 0U; i < PLC_MBW_BLOCKS; i++) {
        if ((s_draft_mask & (1U << i)) != 0U) {
            quiesce(i);
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

void plc_mbw_discard_draft(void)
{
    s_draft_mask = 0U;
}

static void put_regs(uint8_t *out, const uint16_t *regs, unsigned n)
{
    for (unsigned i = 0U; i < n; i++) {
        out[2U * i]      = (uint8_t)(regs[i] >> 8);
        out[2U * i + 1U] = (uint8_t)(regs[i] & 0xFFU);
    }
}

static void get_regs(const uint8_t *in, uint16_t *regs, unsigned n)
{
    for (unsigned i = 0U; i < n; i++) {
        regs[i] = (uint16_t)(((uint16_t)in[2U * i] << 8) | in[2U * i + 1U]);
    }
}

void plc_mbw_export(uint8_t *out)
{
    for (unsigned b = 0U; b < PLC_MBW_BLOCKS; b++) {
        put_regs(&out[b * PLC_MBW_BLOCK_REGS * 2U], s_run[b], PLC_MBW_BLOCK_REGS);
    }
}

bool plc_mbw_import(const uint8_t *in)
{
    uint16_t blk[PLC_MBW_BLOCK_REGS];   /* 128 B on the stack: no 512 B static copy of the image */
    for (unsigned i = 0U; i < PLC_MBW_BLOCKS; i++) {
        get_regs(&in[i * PLC_MBW_BLOCK_REGS * 2U], blk, PLC_MBW_BLOCK_REGS);
        if (!block_valid(blk, i)) { return false; }                 /* nothing applied yet */
    }
    for (unsigned i = 0U; i < PLC_MBW_BLOCKS; i++) {
        quiesce(i);
        get_regs(&in[i * PLC_MBW_BLOCK_REGS * 2U], s_run[i], PLC_MBW_BLOCK_REGS);
        rt_reset(i);
    }
    s_draft_mask = 0U;
    return true;
}

/* ------------------------------------------------------------------------ */
/* Encoding of a pin: tag -> register(s)                                    */

static void put32(uint16_t *r, uint32_t v, uint16_t en)
{
    const uint16_t hi = (uint16_t)(v >> 16), lo = (uint16_t)(v & 0xFFFFU);
    switch (en) {
    case EN_CDAB: r[0] = lo;         r[1] = hi;         break;
    case EN_BADC: r[0] = swap16(hi); r[1] = swap16(lo); break;
    case EN_DCBA: r[0] = swap16(lo); r[1] = swap16(hi); break;
    default:      r[0] = hi;         r[1] = lo;         break;   /* ABCD */
    }
}

/* Write the registers of one pin into vals[]. false = the value cannot be sent
 * (NaN/Inf): nothing may be written. */
static bool encode_pin(const uint16_t *pin, uint16_t *vals)
{
    const uint16_t tag = pin[P_TAG], off = pin[P_OFFSET], dt = pin[P_TYPE], en = pin[P_ENDIAN];
    const int32_t  raw = tag_read(tag);
    const bool     src_real = tag_is_real(tag);

    float mult = bits_to_float(get_u32(&pin[P_MULT]));
    const bool unity = (mult == 0.0f) || (mult == 1.0f);
    if (mult == 0.0f) { mult = 1.0f; }                      /* the App treats 0 as "no scaling" */

    /* The value as a float, scaled; only built when it is needed. */
    float fv = 0.0f;
    if (dt == DT_REAL || src_real || !unity) {
        fv = (src_real ? bits_to_float((uint32_t)raw) : (float)raw) * mult;
        if (!float_finite(fv)) { return false; }
    }

    switch (dt) {
    case DT_BOOL: {
        const bool on = src_real ? (bits_to_float((uint32_t)raw) != 0.0f) : (raw != 0);
        vals[off] = on ? 1U : 0U;
        return true;
    }
    case DT_INT16:
    case DT_UINT16: {
        int32_t iv = (!src_real && unity) ? raw : f2i(fv);
        if (dt == DT_INT16) {
            if (iv < -32768) { iv = -32768; } else if (iv > 32767) { iv = 32767; }
        } else {
            if (iv < 0) { iv = 0; } else if (iv > 65535) { iv = 65535; }
        }
        uint16_t r = (uint16_t)iv;
        if (en == EN_BADC || en == EN_DCBA) { r = swap16(r); }
        vals[off] = r;
        return true;
    }
    case DT_INT32: {
        const int32_t iv = (!src_real && unity) ? raw : f2i(fv);
        put32(&vals[off], (uint32_t)iv, en);
        return true;
    }
    case DT_UINT32: {
        const uint32_t uv = (!src_real && unity) ? (uint32_t)raw : f2u(fv);   /* a tag above INT32_MAX wrapped into the int32 slot */
        put32(&vals[off], uv, en);
        return true;
    }
    default: {   /* DT_REAL */
        put32(&vals[off], float_to_bits(fv), en);
        return true;
    }
    }
}

/* The frame the block sends NOW. For ChannelIndexed it depends on the job phase.
 * false = a REAL source is NaN/Inf. */
static bool build_step(unsigned i, uint8_t *fc, uint16_t *addr, uint16_t *vals, uint16_t *qty)
{
    const uint16_t *b = s_run[i];
    blk_rt_t *r = &s_rt[i];

    if (blk_mode(b) == MODE_DIRECT) {
        *fc   = blk_fc(b);
        *addr = b[F_START];
        *qty  = b[F_COUNT];
        memset(vals, 0, (size_t)(*qty) * sizeof(uint16_t));        /* a register no pin covers is written 0 */
        for (unsigned k = 0U; k < PLC_MBW_MAX_PINS; k++) {
            const uint16_t *p = &b[F_PAYLOAD + k * PIN_REGS];
            if (p[P_TAG] != TAG_NONE_IDX && !encode_pin(p, vals)) { return false; }
        }
        return true;
    }

    const uint16_t base_cmd = b[F_START];
    const uint16_t base_par = (uint16_t)get_u32(&b[F_PARAM_BASE]);
    for (;;) {
        if (r->phase == PH_CLEAR) {
            uint32_t others = 0U;
            if (blk_exclusive(b) && r->job_cmd != 0U) { others = r->lit & ~(1UL << (r->job_ch - 1U)); }
            if (others != 0U) {
                unsigned c = 0U;
                while (((others >> c) & 1U) == 0U) { c++; }
                r->step_ch = (uint8_t)(c + 1U);
                *fc = 6U; *addr = (uint16_t)(base_cmd + c); vals[0] = 0U; *qty = 1U;
                return true;
            }
            r->phase = PH_PARAM;
            continue;
        }
        if (r->phase == PH_PARAM) {
            if (b[F_PARAM_TAG] != TAG_NONE_IDX && r->job_cmd != 0U) {
                *fc = 16U; *addr = (uint16_t)(base_par + (r->job_ch - 1U) * 2U);
                vals[0] = (uint16_t)(r->job_param >> 16);
                vals[1] = (uint16_t)(r->job_param & 0xFFFFU);
                *qty = 2U;
                return true;
            }
            r->phase = PH_CMD;
            continue;
        }
        *fc = 6U; *addr = (uint16_t)(base_cmd + (r->job_ch - 1U)); vals[0] = r->job_cmd; *qty = 1U;
        return true;
    }
}

/* A frame was answered: advance the job. true = the job is complete. */
static bool step_ok(unsigned i)
{
    const uint16_t *b = s_run[i];
    blk_rt_t *r = &s_rt[i];

    if (blk_mode(b) == MODE_DIRECT) { return true; }
    switch (r->phase) {
    case PH_CLEAR:
        r->lit &= ~(1UL << (r->step_ch - 1U));
        return false;
    case PH_PARAM:
        r->phase = PH_CMD;
        return false;
    default:
        if (r->job_cmd != 0U) { r->lit |=  (1UL << (r->job_ch - 1U)); }
        else                  { r->lit &= ~(1UL << (r->job_ch - 1U)); }
        return true;
    }
}

/* ------------------------------------------------------------------------ */

static void finish_ok(unsigned i)
{
    const uint16_t *b = s_run[i];
    blk_rt_t *r = &s_rt[i];
    write_status(b[F_BUSY_TAG], 0);
    write_status(b[F_ERROR_TAG], 0);
    write_status(b[F_ERRCODE_TAG], ERR_NONE);
    write_status(b[F_DONE_TAG], 1);
    r->done_clear = (b[F_DONE_TAG] != TAG_NONE_IDX);
    r->state = ST_IDLE;
}

static void finish_err(unsigned i, int32_t code)
{
    const uint16_t *b = s_run[i];
    blk_rt_t *r = &s_rt[i];
    write_status(b[F_BUSY_TAG], 0);
    write_status(b[F_DONE_TAG], 0);
    write_status(b[F_ERROR_TAG], 1);
    write_status(b[F_ERRCODE_TAG], code);
    r->done_clear = false;
    r->state = ST_IDLE;
    if (blk_mode(b) == MODE_DIRECT) { r->baselined = false; }       /* on-change: write again after the interval */
}

static int32_t code_of(modbus_master_error_t e)
{
    switch (e) {
    case MODBUS_MASTER_ERR_CRC:       return ERR_CRC;
    case MODBUS_MASTER_ERR_FRAME:     return ERR_CRC;            /* a broken answer */
    case MODBUS_MASTER_ERR_EXCEPTION: return ERR_EXCEPTION;
    default:                          return ERR_TIMEOUT;        /* timeout, failed TX */
    }
}

/* The values that make a "change" of the block (Direct: its pin tags; Channel: channel, command, parameter). */
static void snapshot(const uint16_t *b, int32_t *cur)
{
    memset(cur, 0, PLC_MBW_MAX_PINS * sizeof(int32_t));
    if (blk_mode(b) == MODE_DIRECT) {
        for (unsigned k = 0U; k < PLC_MBW_MAX_PINS; k++) {
            const uint16_t t = b[F_PAYLOAD + k * PIN_REGS + P_TAG];
            cur[k] = (t != TAG_NONE_IDX) ? tag_read(t) : 0;
        }
    } else {
        cur[0] = (b[F_CH_TAG]    != TAG_NONE_IDX) ? tag_read(b[F_CH_TAG])    : 0;
        cur[1] = (b[F_CMD_TAG]   != TAG_NONE_IDX) ? tag_read(b[F_CMD_TAG])   : 1;
        cur[2] = (b[F_PARAM_TAG] != TAG_NONE_IDX) ? tag_read(b[F_PARAM_TAG]) : 0;
    }
}

static bool trigger_due(unsigned i, uint32_t now_ms)
{
    const uint16_t *b = s_run[i];
    blk_rt_t *r = &s_rt[i];
    const uint32_t interval = get_u32(&b[F_INTERVAL]);

    switch (b[F_TRIG_MODE]) {
    case TRIG_EDGE:
        return r->pending;
    case TRIG_CHANGE: {
        int32_t cur[PLC_MBW_MAX_PINS];
        snapshot(b, cur);
        bool changed;
        if (!r->baselined) {
            if (blk_mode(b) == MODE_CHANNEL) {             /* a channel block starts from what the tags hold now */
                memcpy(r->last, cur, sizeof(r->last));
                r->baselined = true;
                return false;
            }
            changed = true;                               /* Direct: write the baseline once */
        } else {
            changed = memcmp(cur, r->last, sizeof(cur)) != 0;
        }
        return changed && (!r->started || (uint32_t)(now_ms - r->last_start_ms) >= interval);
    }
    default:   /* TRIG_CYCLIC */
        return !r->started || (uint32_t)(now_ms - r->last_start_ms) >= interval;
    }
}

static void job_start(unsigned i, uint32_t now_ms)
{
    const uint16_t *b = s_run[i];
    blk_rt_t *r = &s_rt[i];

    r->started       = true;
    r->pending       = false;
    r->last_start_ms = now_ms;
    r->wait_since_ms = now_ms;
    r->phase         = PH_CLEAR;
    r->in_retry      = false;
    snapshot(b, r->last);
    r->baselined     = true;

    write_status(b[F_BUSY_TAG], 1);
    write_status(b[F_ERROR_TAG], 0);
    write_status(b[F_ERRCODE_TAG], ERR_NONE);
    write_status(b[F_DONE_TAG], 0);

    if (blk_mode(b) == MODE_CHANNEL) {
        const int32_t ch = r->last[0];
        if (ch < 1 || ch > (int32_t)b[F_COUNT]) {
            log_warn(TAG, "block %u: channel %ld outside 1..%u", i, (long)ch, b[F_COUNT]);
            finish_err(i, ERR_CHANNEL);
            return;
        }
        int32_t cmd = r->last[1];
        if (cmd < 0) { cmd = 0; } else if (cmd > 65535) { cmd = 65535; }
        r->job_ch    = (uint16_t)ch;
        r->job_cmd   = (uint16_t)cmd;
        r->job_param = (uint32_t)r->last[2];
    }
    r->state = ST_SEND;
}

static void try_send(unsigned i, uint32_t now_ms)
{
    const uint16_t *b = s_run[i];
    blk_rt_t *r = &s_rt[i];
    const unsigned port = blk_port(b);
    modbus_rtu_master_t *m = NULL;
    uint8_t retries = 0U;

    switch (plc_mbm_port_claim(port, &m, &retries)) {
    case PLC_MBM_CLAIM_UNUSABLE:
        if (!r->warned_port) {
            r->warned_port = true;
            log_warn(TAG, "block %u: port %u is not registered or disabled, the block reports err_code 1", i, port + 1U);
        }
        finish_err(i, ERR_TIMEOUT);
        return;
    case PLC_MBM_CLAIM_BUSY:
        if ((uint32_t)(now_ms - r->wait_since_ms) >= BUS_WAIT_MS) { finish_err(i, ERR_BUS_BUSY); }
        return;
    default:
        break;
    }

    uint8_t  fc;
    uint16_t addr, qty;
    uint16_t vals[PLC_MBW_MAX_COUNT];
    if (!build_step(i, &fc, &addr, vals, &qty)) {
        plc_mbm_port_release(port, false);
        log_warn(TAG, "block %u: a REAL source value is NaN/Inf, nothing sent", i);
        finish_err(i, ERR_BAD_VALUE);
        return;
    }
    if (!r->in_retry) { r->retries_left = retries; }

    if (modbus_rtu_master_write_fc(m, blk_slave(b), fc, addr, vals, qty, get_u32(&b[F_TIMEOUT]))) {
        r->in_retry = false;
        r->m        = m;
        r->port     = (uint8_t)port;
        r->state    = ST_WAIT;
    } else {
        plc_mbm_port_release(port, false);       /* inter-frame gap not elapsed: try again next scan */
    }
}

static void collect(unsigned i, uint32_t now_ms)
{
    blk_rt_t *r = &s_rt[i];
    const modbus_master_state_t st = modbus_rtu_master_poll(r->m);
    if (st == MODBUS_MASTER_BUSY || st == MODBUS_MASTER_IDLE) { return; }
    const modbus_master_error_t err = modbus_rtu_master_error(r->m);
    plc_mbm_port_release(r->port, true);

    if (r->abandon) {                              /* its block was replaced meanwhile */
        memset(r, 0, sizeof(*r));
        return;
    }
    if (st == MODBUS_MASTER_DONE_OK) {
        r->in_retry = false;
        if (step_ok(i)) { finish_ok(i); }
        else            { r->state = ST_SEND; r->wait_since_ms = now_ms; }
        return;
    }
    /* A slave exception is an answer: do not repeat it. Timeouts, CRC errors and broken
     * frames are repeated as often as the port was configured to. */
    if (err != MODBUS_MASTER_ERR_EXCEPTION && r->retries_left > 0U) {
        r->retries_left--;
        r->in_retry      = true;
        r->state         = ST_SEND;
        r->wait_since_ms = now_ms;
        return;
    }
    finish_err(i, code_of(err));
}

void plc_mbw_scan(uint32_t now_ms)
{
    /* The done tag is a one-scan pulse. */
    for (unsigned i = 0U; i < PLC_MBW_BLOCKS; i++) {
        if (s_rt[i].done_clear) {
            write_status(s_run[i][F_DONE_TAG], 0);
            s_rt[i].done_clear = false;
        }
    }

    /* Rising edge of every edge-triggered block's tag, even while a job runs. */
    for (unsigned i = 0U; i < PLC_MBW_BLOCKS; i++) {
        const uint16_t *b = s_run[i];
        if (blk_enabled(b) && b[F_TRIG_MODE] == TRIG_EDGE && !s_rt[i].abandon) {
            const bool now = tag_read(b[F_TRIG_TAG]) != 0;
            if (now && !s_rt[i].prev_trig) { s_rt[i].pending = true; }
            s_rt[i].prev_trig = now;
        }
    }

    /* Frames in flight (also those of a block that was replaced meanwhile). */
    for (unsigned i = 0U; i < PLC_MBW_BLOCKS; i++) {
        if (s_rt[i].state == ST_WAIT) { collect(i, now_ms); }
    }

    /* Start jobs / frames: at most one frame per block; blocks take turns. */
    for (unsigned n = 0U; n < PLC_MBW_BLOCKS; n++) {
        const unsigned i = (s_rr + n) % PLC_MBW_BLOCKS;
        blk_rt_t *r = &s_rt[i];
        if (!blk_enabled(s_run[i]) || r->abandon) { continue; }
        if (r->state == ST_IDLE) {
            if (!trigger_due(i, now_ms)) { continue; }
            job_start(i, now_ms);
        }
        if (r->state == ST_SEND) { try_send(i, now_ms); }
    }
    s_rr = (uint8_t)((s_rr + 1U) % PLC_MBW_BLOCKS);
}

#endif /* SPLC_MBW_ENABLE */