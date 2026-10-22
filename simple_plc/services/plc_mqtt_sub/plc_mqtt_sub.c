/*
 * plc_mqtt_sub.c - Layer 3. See plc_mqtt_sub.h.
 */
#include "plc_mqtt_sub.h"

#if SPLC_MQTT_SUB_ENABLE

#include <string.h>

#include "plc_tag.h"
#include "splc_log.h"
#if SPLC_REAL_ENABLE
#include "plc_tagtype.h"
#endif

static const char *TAG = "PLC_MQTT_SUB";

#define BLOCKS   PLC_MQTT_SUB_BLOCKS
#define REGS     PLC_MQTT_SUB_BLOCK_REGS
#define DEPTH    ((unsigned)SPLC_MQTT_SUB_FIFO_DEPTH)

/* --- block field offsets (register index inside a block) ----------------- */
#define F_FLAGS      0U
#define F_TIMEOUT    2U    /* 2 regs, High Word first */
#define F_PIN_COUNT  4U
#define F_TOPIC      8U    /* 16 regs = 32 chars */
#define TOPIC_REGS   16U
#define F_PINS       24U
#define PIN_REGS     10U
#define P_TAG        0U
#define P_TYPE       1U
#define P_DEFAULT    2U    /* 2 regs, High Word first */
#define P_KEY        4U    /* 6 regs = 12 chars */
#define KEY_REGS     6U
#define KEY_CHARS    12U

/* F_FLAGS */
#define FLAG_ENABLED     0x0001U
#define FLAG_QOS_MASK    0x0006U
#define FLAG_QOS_SHIFT   1U
#define FLAG_FIFO        0x0008U
#define FLAG_IGN_RETAIN  0x0010U
#define FLAG_FS_MASK     0x0060U
#define FLAG_FS_SHIFT    5U

#define TAG_NONE_IDX   0xFFFFU
#define JSON_MAX_DEPTH 16U

enum { DT_BOOL = 1, DT_INT32, DT_REAL };
enum { FS_KEEP = 0, FS_ZERO, FS_DEFAULT };

/* A value read from a payload (or from a default): an integer or a float. */
typedef struct {
    bool    is_real;
    int32_t i;
    float   f;
} sval_t;

typedef struct {
    uint8_t head;                               /* oldest message */
    uint8_t count;
    uint8_t len[DEPTH];
    char    buf[DEPTH][PLC_MQTT_SUB_PAYLOAD_MAX];
} queue_t;

typedef struct {
    bool     armed;        /* the watchdog started counting */
    bool     fs_fired;     /* the fail-safe action ran and no message arrived since */
    uint32_t last_rx_ms;
} block_rt_t;

static uint16_t   s_run[BLOCKS][REGS];     /* RUNNING */
static uint16_t   s_draft[BLOCKS][REGS];   /* valid only where the mask bit is set */
static uint8_t    s_draft_mask;
static queue_t    s_q[BLOCKS];
static block_rt_t s_rt[BLOCKS];
static uint32_t   s_gen;
static splc_mqtt_sub_stats_t s_stats;

/* ------------------------------------------------------------------------ */
/* Small helpers                                                            */

static uint32_t get_u32(const uint16_t *r) { return ((uint32_t)r[0] << 16) | r[1]; }

static bool blk_enabled(const uint16_t *b) { return (b[F_FLAGS] & FLAG_ENABLED) != 0U; }
static uint8_t blk_qos(const uint16_t *b)  { return (uint8_t)((b[F_FLAGS] & FLAG_QOS_MASK) >> FLAG_QOS_SHIFT); }
static unsigned blk_fs(const uint16_t *b)  { return (unsigned)((b[F_FLAGS] & FLAG_FS_MASK) >> FLAG_FS_SHIFT); }

static float bits_to_float(uint32_t u) { float f; memcpy(&f, &u, sizeof(f)); return f; }
#if SPLC_REAL_ENABLE
static uint32_t float_to_bits(float f) { uint32_t u; memcpy(&u, &f, sizeof(u)); return u; }
#endif

/* Round to nearest, saturating. No libm. */
static int32_t f2i(float x)
{
    if (x >= 2147483520.0f)  { return INT32_MAX; }
    if (x <= -2147483648.0f) { return INT32_MIN; }
    return (int32_t)((x >= 0.0f) ? (x + 0.5f) : (x - 0.5f));
}

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c; }
static bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static bool is_digit(char c) { return c >= '0' && c <= '9'; }

/* Characters of a register string (two per register, first in the high byte)
 * into `out`, stopping at the first NUL or after `max` characters. */
static size_t regs_to_str(const uint16_t *r, size_t nregs, char *out, size_t max)
{
    size_t n = 0U;
    for (size_t i = 0U; i < nregs * 2U && n < max; i++) {
        const char c = (char)((i & 1U) ? (r[i / 2U] & 0xFFU) : (r[i / 2U] >> 8));
        if (c == '\0') { break; }
        out[n++] = c;
    }
    out[n] = '\0';
    return n;
}

static void block_topic(const uint16_t *b, char *out /* TOPIC_MAX + 1 */)
{
    (void)regs_to_str(&b[F_TOPIC], TOPIC_REGS, out, PLC_MQTT_SUB_TOPIC_MAX);
}

/* ------------------------------------------------------------------------ */
/* Decision 47: which tags a message may write.                             */

/* The firmware does not restrict the KIND of a target tag (it follows the App,
 * which sets no restriction): any tag that exists. Change ONLY this function to
 * restrict the kinds later (e.g. refuse TAG_DO, or accept only VFLAG / VREG). */
static bool target_kind_ok(SPLC_TagKind k)
{
    return k != TAG_NONE;
}

static bool target_ok(uint16_t idx)
{
    return idx == TAG_NONE_IDX || (idx < MAX_TAGS && target_kind_ok(tag_get_kind(idx)));
}

/* ------------------------------------------------------------------------ */
/* Topic filters                                                            */

/* MQTT 3.1.1 4.7: '#' only as the whole last level, '+' as a whole level. */
static bool filter_valid(const char *f, size_t len)
{
    if (len == 0U) { return false; }
    for (size_t i = 0U; i < len; i++) {
        const char c = f[i];
        if ((unsigned char)c < 0x20U || (unsigned char)c > 0x7EU) { return false; }
        if (c == '#') {
            if (i + 1U != len || (i > 0U && f[i - 1U] != '/')) { return false; }
        } else if (c == '+') {
            if ((i > 0U && f[i - 1U] != '/') || (i + 1U < len && f[i + 1U] != '/')) { return false; }
        }
    }
    return true;
}

/* `filter` and `topic` are NUL-terminated; the filter was validated. */
static bool topic_match(const char *f, const char *t)
{
    if (t[0] == '$' && (f[0] == '#' || f[0] == '+')) { return false; }   /* MQTT 4.7.2 */
    for (;;) {
        if (*f == '#') { return true; }
        if (*f == '+') {
            f++;
            while (*t != '\0' && *t != '/') { t++; }
        } else {
            while (*f != '\0' && *f != '/' && *t != '\0' && *t != '/') {
                if (*f != *t) { return false; }
                f++; t++;
            }
            if ((*f != '\0' && *f != '/') || (*t != '\0' && *t != '/')) { return false; }
        }
        if (*f == '\0' && *t == '\0') { return true; }
        if (*f == '/' && *t == '/')   { f++; t++; continue; }
        if (*f == '/' && f[1] == '#' && f[2] == '\0' && *t == '\0') { return true; }   /* "a/#" matches "a" */
        return false;
    }
}

/* ------------------------------------------------------------------------ */
/* Validation of one block                                                  */

static bool block_valid(const uint16_t *b, unsigned idx)
{
    if (!blk_enabled(b)) {
        return true;                       /* the App writes 0 into a block it no longer uses */
    }
    if (blk_qos(b) > 2U)                   { log_warn(TAG, "block %u: QoS %u", idx, (unsigned)blk_qos(b)); return false; }
    if (blk_fs(b) > FS_DEFAULT)            { log_warn(TAG, "block %u: fail-safe action %u", idx, blk_fs(b)); return false; }

    char topic[PLC_MQTT_SUB_TOPIC_MAX + 1U];
    const size_t tl = regs_to_str(&b[F_TOPIC], TOPIC_REGS, topic, PLC_MQTT_SUB_TOPIC_MAX);
    if (!filter_valid(topic, tl))          { log_warn(TAG, "block %u: topic filter not valid", idx); return false; }

    if (b[F_PIN_COUNT] < 1U || b[F_PIN_COUNT] > PLC_MQTT_SUB_MAX_PINS) {
        log_warn(TAG, "block %u: pin count %u", idx, b[F_PIN_COUNT]); return false;
    }
    for (uint16_t i = 0U; i < b[F_PIN_COUNT]; i++) {
        const uint16_t *p = &b[F_PINS + i * PIN_REGS];
        if (!target_ok(p[P_TAG]))          { log_warn(TAG, "block %u pin %u: target tag %u does not exist", idx, i, p[P_TAG]); return false; }
        if (p[P_TYPE] < DT_BOOL || p[P_TYPE] > DT_REAL) {
            log_warn(TAG, "block %u pin %u: data type %u", idx, i, p[P_TYPE]); return false;
        }
        char key[KEY_CHARS + 1U];
        const size_t kl = regs_to_str(&p[P_KEY], KEY_REGS, key, KEY_CHARS);
        for (size_t k = 0U; k < kl; k++) {
            if ((unsigned char)key[k] < 0x20U || (unsigned char)key[k] > 0x7EU) {
                log_warn(TAG, "block %u pin %u: key not printable", idx, i); return false;
            }
        }
    }
    return true;
}

/* ------------------------------------------------------------------------ */
/* Number / word parsing (no libc strtod: bounded, no locale, no heap)      */

typedef struct {
    bool    integral;   /* the literal had no '.' and no exponent */
    int32_t iv;         /* valid when integral (saturated to int32) */
    float   fv;
} num_t;

static const float P10[11] = { 1e0f, 1e1f, 1e2f, 1e3f, 1e4f, 1e5f, 1e6f, 1e7f, 1e8f, 1e9f, 1e10f };

/* Decimal literal [+-]digits[.digits][(e|E)[+-]digits], the whole of s[0..n).
 * Accuracy (measured on the PC against strtof, docs/handoff.md decision 47):
 * EXACT (correctly rounded, identical to the App's float parse) when the literal has
 * at most 7 significant digits and the decimal exponent is within -10..10 -- every
 * ordinary setpoint ("12.3", "101.325", "0.0001", "2.5e3"). Beyond that it is 1 ulp
 * off for about a quarter of the literals with 8-9 digits (the mantissa is rounded
 * to float before the power of ten is applied) and up to 3 ulp off with an exponent
 * outside -10..10 (the power of ten is built from 1e10 steps). That is far below the
 * 1e-5 tolerance of the rule engine, but a bit-exact match with the App is not
 * promised there. A value beyond float range is refused. */
static bool parse_num(const char *s, size_t n, num_t *o)
{
    size_t i = 0U;
    bool neg = false, any = false, has_frac = false, has_exp = false;
    uint32_t mant = 0U;
    uint64_t iacc = 0U;
    int dexp = 0;

    if (i < n && (s[i] == '+' || s[i] == '-')) { neg = (s[i] == '-'); i++; }
    while (i < n && is_digit(s[i])) {
        const uint32_t d = (uint32_t)(s[i] - '0');
        any = true;
        if (mant <= 99999999UL) { mant = mant * 10U + d; } else { dexp++; }
        if (iacc < 3000000000ULL) { iacc = iacc * 10U + d; }
        i++;
    }
    if (i < n && s[i] == '.') {
        has_frac = true;
        i++;
        while (i < n && is_digit(s[i])) {
            const uint32_t d = (uint32_t)(s[i] - '0');
            any = true;
            if (mant <= 99999999UL) { mant = mant * 10U + d; dexp--; }
            i++;
        }
    }
    if (!any) { return false; }
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        has_exp = true;
        i++;
        bool eneg = false;
        if (i < n && (s[i] == '+' || s[i] == '-')) { eneg = (s[i] == '-'); i++; }
        int ex = 0;
        bool edig = false;
        while (i < n && is_digit(s[i])) {
            edig = true;
            if (ex < 400) { ex = ex * 10 + (s[i] - '0'); }
            i++;
        }
        if (!edig) { return false; }
        dexp += eneg ? -ex : ex;
    }
    if (i != n) { return false; }

    float f = (float)mant;
    if (mant != 0U) {
        int e = dexp;
        if (e > 0) {
            while (e > 10) { f *= 1e10f; e -= 10; if (f > 3.4028235e38f) { return false; } }
            f *= P10[e];
        } else if (e < 0) {
            e = -e;
            while (e > 10 && f != 0.0f) { f /= 1e10f; e -= 10; }
            if (f != 0.0f) { f /= P10[e > 10 ? 10 : e]; }
        }
        if (f > 3.4028235e38f) { return false; }       /* too big for a float (also catches Inf) */
    }
    o->fv = neg ? -f : f;
    o->integral = !has_frac && !has_exp;
    if (neg) { o->iv = (iacc > 2147483648ULL) ? INT32_MIN : (int32_t)(-(int64_t)iacc); }
    else     { o->iv = (iacc > 2147483647ULL) ? INT32_MAX : (int32_t)iacc; }
    return true;
}

static bool word_eq(const char *s, size_t n, const char *w)
{
    size_t i = 0U;
    for (; i < n && w[i] != '\0'; i++) { if (lc(s[i]) != w[i]) { return false; } }
    return i == n && w[i] == '\0';
}

/* true / false / on / off (any case). */
static bool parse_bool_word(const char *s, size_t n, bool *out)
{
    if (word_eq(s, n, "true") || word_eq(s, n, "on"))   { *out = true;  return true; }
    if (word_eq(s, n, "false") || word_eq(s, n, "off")) { *out = false; return true; }
    return false;
}

/* ------------------------------------------------------------------------ */
/* Payload scanning (zero allocation)                                       */

typedef struct { const char *p; size_t n; } tok_t;

static const char *skip_ws(const char *p, const char *e)
{
    while (p < e && is_ws(*p)) { p++; }
    return p;
}

/* p at an opening quote: the pointer just after the closing quote, or NULL. */
static const char *skip_string(const char *p, const char *e)
{
    p++;
    while (p < e) {
        if (*p == '\\') { p += 2; continue; }
        if (*p == '"')  { return p + 1; }
        p++;
    }
    return NULL;
}

/* p at the first character of a value: the pointer just after it, or NULL when
 * the value is malformed or nested deeper than JSON_MAX_DEPTH. */
static const char *skip_value(const char *p, const char *e)
{
    if (p >= e) { return NULL; }
    if (*p == '"') { return skip_string(p, e); }
    if (*p == '{' || *p == '[') {
        unsigned depth = 0U;
        while (p < e) {
            const char c = *p;
            if (c == '"') {
                p = skip_string(p, e);
                if (p == NULL) { return NULL; }
                continue;
            }
            if (c == '{' || c == '[') {
                if (++depth > JSON_MAX_DEPTH) { return NULL; }
            } else if (c == '}' || c == ']') {
                if (--depth == 0U) { return p + 1; }
            }
            p++;
        }
        return NULL;
    }
    while (p < e && *p != ',' && *p != '}' && *p != ']' && !is_ws(*p)) { p++; }
    return p;
}

/* Key match: case-insensitive, '_' and '-' ignored (ICD 4.3). */
static bool key_eq(const char *a, size_t an, const char *b, size_t bn)
{
    size_t i = 0U, j = 0U;
    for (;;) {
        while (i < an && (a[i] == '_' || a[i] == '-')) { i++; }
        while (j < bn && (b[j] == '_' || b[j] == '-')) { j++; }
        if (i == an || j == bn) { return i == an && j == bn; }
        if (lc(a[i]) != lc(b[j])) { return false; }
        i++; j++;
    }
}

/* p at '{': the start of the value of member `key`, or NULL. */
static const char *obj_member(const char *p, const char *e, const char *key, size_t klen)
{
    p = skip_ws(p + 1, e);
    if (p < e && *p == '}') { return NULL; }
    for (;;) {
        p = skip_ws(p, e);
        if (p >= e || *p != '"') { return NULL; }
        const char *ks = p + 1;
        const char *ke = skip_string(p, e);
        if (ke == NULL) { return NULL; }
        const size_t kl = (size_t)(ke - 1 - ks);
        p = skip_ws(ke, e);
        if (p >= e || *p != ':') { return NULL; }
        p = skip_ws(p + 1, e);
        if (key_eq(ks, kl, key, klen)) { return p; }
        p = skip_value(p, e);
        if (p == NULL) { return NULL; }
        p = skip_ws(p, e);
        if (p < e && *p == ',') { p++; continue; }
        return NULL;
    }
}

/* p at '[': the start of element `idx`, or NULL. */
static const char *arr_index(const char *p, const char *e, uint32_t idx)
{
    p = skip_ws(p + 1, e);
    if (p < e && *p == ']') { return NULL; }
    for (uint32_t n = 0U;; n++) {
        p = skip_ws(p, e);
        if (p >= e) { return NULL; }
        if (n == idx) { return p; }
        p = skip_value(p, e);
        if (p == NULL) { return NULL; }
        p = skip_ws(p, e);
        if (p < e && *p == ',') { p++; continue; }
        return NULL;
    }
}

/* Walk `path` ("a.b[1].c", NUL-terminated) from the root value at p. */
static const char *json_lookup(const char *p, const char *e, const char *path)
{
    p = skip_ws(p, e);
    while (*path != '\0') {
        const char *seg = path;
        while (*path != '\0' && *path != '.' && *path != '[') { path++; }
        const size_t sl = (size_t)(path - seg);
        if (sl > 0U) {
            if (p >= e || *p != '{') { return NULL; }
            p = obj_member(p, e, seg, sl);
            if (p == NULL) { return NULL; }
        }
        while (*path == '[') {
            path++;
            uint32_t idx = 0U;
            bool any = false;
            while (is_digit(*path)) {
                idx = idx * 10U + (uint32_t)(*path - '0');
                any = true;
                path++;
                if (idx > 1000U) { return NULL; }
            }
            if (!any || *path != ']') { return NULL; }
            path++;
            if (p >= e || *p != '[') { return NULL; }
            p = arr_index(p, e, idx);
            if (p == NULL) { return NULL; }
        }
        if (*path == '.') { path++; }
        else if (*path != '\0') { return NULL; }
    }
    return p;
}

/* The scalar that starts at p: a number / word, or the inside of a string. */
static bool scalar_token(const char *p, const char *e, tok_t *t)
{
    if (p >= e) { return false; }
    if (*p == '{' || *p == '[') { return false; }
    if (*p == '"') {
        const char *q = skip_string(p, e);
        if (q == NULL) { return false; }
        t->p = p + 1;
        t->n = (size_t)(q - 1 - (p + 1));
    } else {
        const char *q = p;
        while (q < e && *q != ',' && *q != '}' && *q != ']' && !is_ws(*q)) { q++; }
        t->p = p;
        t->n = (size_t)(q - p);
    }
    while (t->n > 0U && is_ws(t->p[t->n - 1U])) { t->n--; }     /* "  12 " inside a string */
    while (t->n > 0U && is_ws(*t->p)) { t->p++; t->n--; }
    return t->n > 0U;
}

/* i-th value of a plain-text list "5, 1;2" (separators , ; tab; empty entries skipped). */
static bool csv_token(const char *p, const char *e, uint32_t idx, tok_t *t)
{
    uint32_t n = 0U;
    while (p < e) {
        while (p < e && (is_ws(*p) || *p == ',' || *p == ';')) { p++; }
        if (p >= e) { break; }
        const char *s = p;
        while (p < e && *p != ',' && *p != ';' && *p != '\t') { p++; }
        const char *q = p;
        while (q > s && is_ws(q[-1])) { q--; }
        if (q > s) {
            if (n == idx) { t->p = s; t->n = (size_t)(q - s); return true; }
            n++;
        }
    }
    return false;
}

static bool key_is_pos(const char *key, uint32_t *idx)
{
    if (!(lc(key[0]) == 'p' && lc(key[1]) == 'o' && lc(key[2]) == 's' && key[3] == '[')) { return false; }
    uint32_t v = 0U;
    const char *c = &key[4];
    if (!is_digit(*c)) { return false; }
    while (is_digit(*c)) { v = v * 10U + (uint32_t)(*c - '0'); c++; if (v > 1000U) { return false; } }
    if (c[0] != ']' || c[1] != '\0') { return false; }
    *idx = v;
    return true;
}

/* Find the value a pin asks for. */
static bool extract(const char *pl, size_t n, const char *key, tok_t *t)
{
    const char *e = pl + n;
    const char *r = skip_ws(pl, e);
    uint32_t pidx;

    if (key[0] == '\0' || key_eq(key, strlen(key), "raw", 3U)) {      /* the whole payload */
        return scalar_token(r, e, t);
    }
    if (r < e && (*r == '{' || *r == '[')) {                           /* JSON */
        const char *path = key;
        if (*r == '[' && key_is_pos(key, &pidx)) { path = &key[3]; }   /* "pos[1]" on a root array = "[1]" */
        const char *v = json_lookup(r, e, path);
        return v != NULL && scalar_token(v, e, t);
    }
    if (key_is_pos(key, &pidx)) {                                      /* plain text list */
        return csv_token(r, e, pidx, t);
    }
    return false;
}

/* Read one pin's value out of the payload. */
static bool read_pin(const uint16_t *pin, const char *pl, size_t n, sval_t *v)
{
    char key[KEY_CHARS + 1U];
    (void)regs_to_str(&pin[P_KEY], KEY_REGS, key, KEY_CHARS);

    tok_t t;
    if (!extract(pl, n, key, &t)) { return false; }

    bool word = false;
    const bool is_word = parse_bool_word(t.p, t.n, &word);
    num_t num;

    v->is_real = false;
    v->i = 0;
    v->f = 0.0f;
    switch (pin[P_TYPE]) {
    case DT_BOOL:
        if (is_word) { v->i = word ? 1 : 0; return true; }
        if (!parse_num(t.p, t.n, &num)) { return false; }
        v->i = (num.integral ? (num.iv != 0) : (num.fv != 0.0f)) ? 1 : 0;
        return true;
    case DT_INT32:
        if (is_word) { v->i = word ? 1 : 0; return true; }
        if (!parse_num(t.p, t.n, &num)) { return false; }
        v->i = num.integral ? num.iv : f2i(num.fv);
        return true;
    case DT_REAL:
        v->is_real = true;
        if (is_word) { v->f = word ? 1.0f : 0.0f; return true; }
        if (!parse_num(t.p, t.n, &num)) { return false; }
        v->f = num.fv;
        return true;
    default:
        return false;
    }
}

/* ------------------------------------------------------------------------ */
/* Writing the tags                                                         */

static int32_t to_tag_word(uint16_t tag, const sval_t *v)
{
#if SPLC_REAL_ENABLE
    if (plc_tagtype_is_real(tag)) {
        return (int32_t)float_to_bits(v->is_real ? v->f : (float)v->i);
    }
#else
    (void)tag;
#endif
    return v->is_real ? f2i(v->f) : v->i;
}

static void default_value(const uint16_t *pin, sval_t *v)
{
    const uint32_t raw = get_u32(&pin[P_DEFAULT]);
    memset(v, 0, sizeof(*v));
    switch (pin[P_TYPE]) {
    case DT_REAL:
        v->is_real = true;
        v->f = (((raw >> 23) & 0xFFU) == 0xFFU) ? 0.0f : bits_to_float(raw);   /* NaN / Inf -> 0 */
        break;
    case DT_BOOL:
        v->i = (raw != 0U) ? 1 : 0;
        break;
    default:
        v->i = (int32_t)raw;
        break;
    }
}

/* Apply one message to a block: ATOMIC, every pin with a target needs a value. */
static bool apply_message(const uint16_t *b, const char *pl, size_t n)
{
    sval_t vals[PLC_MQTT_SUB_MAX_PINS];
    const unsigned pc = b[F_PIN_COUNT];

    for (unsigned i = 0U; i < pc; i++) {
        const uint16_t *p = &b[F_PINS + i * PIN_REGS];
        if (p[P_TAG] == TAG_NONE_IDX) { continue; }
        if (!read_pin(p, pl, n, &vals[i])) { return false; }
    }
    for (unsigned i = 0U; i < pc; i++) {
        const uint16_t *p = &b[F_PINS + i * PIN_REGS];
        if (p[P_TAG] == TAG_NONE_IDX) { continue; }
        tag_write(p[P_TAG], to_tag_word(p[P_TAG], &vals[i]));
    }
    return true;
}

static void apply_failsafe(const uint16_t *b)
{
    const unsigned act = blk_fs(b);
    if (act == FS_KEEP) { return; }
    for (unsigned i = 0U; i < b[F_PIN_COUNT]; i++) {
        const uint16_t *p = &b[F_PINS + i * PIN_REGS];
        if (p[P_TAG] == TAG_NONE_IDX) { continue; }
        sval_t v;
        if (act == FS_DEFAULT) { default_value(p, &v); }
        else { memset(&v, 0, sizeof(v)); }
        tag_write(p[P_TAG], to_tag_word(p[P_TAG], &v));
    }
}

/* ------------------------------------------------------------------------ */
/* Lifecycle, Modbus access, staging                                        */

static void block_reset(unsigned i)
{
    memset(&s_q[i], 0, sizeof(s_q[i]));
    memset(&s_rt[i], 0, sizeof(s_rt[i]));
}

void plc_mqtt_sub_clear_all(void)
{
    memset(s_run, 0, sizeof(s_run));
    memset(s_draft, 0, sizeof(s_draft));
    s_draft_mask = 0U;
    for (unsigned i = 0U; i < BLOCKS; i++) { block_reset(i); }
    s_gen++;
}

void plc_mqtt_sub_init(void)
{
    memset(&s_stats, 0, sizeof(s_stats));
    plc_mqtt_sub_clear_all();
}

void plc_mqtt_sub_read(uint16_t offset, uint16_t quantity, uint16_t *registers_out)
{
    for (uint16_t i = 0U; i < quantity; i++) {
        const uint16_t o = (uint16_t)(offset + i);
        registers_out[i] = (o < PLC_MQTT_SUB_REGS) ? s_run[o / REGS][o % REGS] : 0U;
    }
}

plc_mqtt_sub_write_result_t plc_mqtt_sub_write(uint16_t offset, uint16_t quantity, const uint16_t *registers)
{
    if ((uint32_t)offset + quantity > PLC_MQTT_SUB_REGS) { return PLC_MQTT_SUB_WRITE_BAD_ADDRESS; }
    for (uint16_t i = 0U; i < quantity; i++) {
        const uint16_t o = (uint16_t)(offset + i);
        const unsigned blk = o / REGS;
        if ((s_draft_mask & (1U << blk)) == 0U) {
            memcpy(s_draft[blk], s_run[blk], sizeof(s_draft[blk]));    /* a partial write keeps the rest */
            s_draft_mask |= (uint8_t)(1U << blk);
        }
        s_draft[blk][o % REGS] = registers[i];
    }
    return PLC_MQTT_SUB_WRITE_OK;
}

bool plc_mqtt_sub_validate_draft(void)
{
    for (unsigned i = 0U; i < BLOCKS; i++) {
        if ((s_draft_mask & (1U << i)) != 0U && !block_valid(s_draft[i], i)) { return false; }
    }
    return true;
}

void plc_mqtt_sub_commit_draft(void)
{
    unsigned enabled = 0U;
    for (unsigned i = 0U; i < BLOCKS; i++) {
        if ((s_draft_mask & (1U << i)) != 0U) {
            memcpy(s_run[i], s_draft[i], sizeof(s_run[i]));
            block_reset(i);
            if (blk_enabled(s_run[i]) && (s_run[i][F_FLAGS] & FLAG_FIFO) != 0U && DEPTH <= 1U) {
                log_warn(TAG, "block %u asks for a FIFO but SPLC_MQTT_SUB_FIFO_DEPTH is 1: latest message only", i);
            }
        }
        if (blk_enabled(s_run[i])) { enabled++; }
    }
    if (s_draft_mask != 0U) {
        s_gen++;                                  /* the host re-subscribes */
        log_info(TAG, "COMMIT: draft_mask=0x%02X, %u block(s) enabled", (unsigned)s_draft_mask, enabled);
    }
    s_draft_mask = 0U;
}

void plc_mqtt_sub_discard_draft(void) { s_draft_mask = 0U; }

void plc_mqtt_sub_export(uint8_t *out)
{
    for (unsigned b = 0U; b < BLOCKS; b++) {
        for (unsigned r = 0U; r < REGS; r++) {
            const uint16_t v = s_run[b][r];
            out[(b * REGS + r) * 2U]      = (uint8_t)(v >> 8);
            out[(b * REGS + r) * 2U + 1U] = (uint8_t)(v & 0xFFU);
        }
    }
}

bool plc_mqtt_sub_import(const uint8_t *in)
{
    uint16_t blk[REGS];                          /* 128 B on the stack: no 256 B static copy */
    for (unsigned b = 0U; b < BLOCKS; b++) {
        for (unsigned r = 0U; r < REGS; r++) {
            blk[r] = (uint16_t)(((uint16_t)in[(b * REGS + r) * 2U] << 8) | in[(b * REGS + r) * 2U + 1U]);
        }
        if (!block_valid(blk, b)) { return false; }                  /* nothing applied yet */
    }
    for (unsigned b = 0U; b < BLOCKS; b++) {
        for (unsigned r = 0U; r < REGS; r++) {
            s_run[b][r] = (uint16_t)(((uint16_t)in[(b * REGS + r) * 2U] << 8) | in[(b * REGS + r) * 2U + 1U]);
        }
        block_reset(b);
    }
    s_draft_mask = 0U;
    s_gen++;
    return true;
}

/* ------------------------------------------------------------------------ */
/* Host side                                                                */

bool splc_mqtt_sub_get(uint8_t block, char *topic_out, uint8_t *qos_out)
{
    if (block >= BLOCKS || !blk_enabled(s_run[block])) { return false; }
    if (topic_out != NULL) { block_topic(s_run[block], topic_out); }
    if (qos_out != NULL)   { *qos_out = blk_qos(s_run[block]); }
    return true;
}

uint32_t splc_mqtt_sub_generation(void) { return s_gen; }

void splc_mqtt_sub_get_stats(splc_mqtt_sub_stats_t *out)
{
    if (out != NULL) { *out = s_stats; }
}

static void enqueue(unsigned blk, const char *payload, size_t len)
{
    queue_t *q = &s_q[blk];
    const bool fifo = (s_run[blk][F_FLAGS] & FLAG_FIFO) != 0U && DEPTH > 1U;
    unsigned slot;

    if (!fifo) {                                  /* latest only: the new message replaces the old one */
        q->head = 0U;
        q->count = 1U;
        slot = 0U;
    } else {
        if (q->count >= DEPTH) { s_stats.dropped++; return; }        /* full: the newest is dropped */
        slot = ((unsigned)q->head + q->count) % DEPTH;
        q->count++;
    }
    if (len > 0U) { memcpy(q->buf[slot], payload, len); }
    q->len[slot] = (uint8_t)len;
}

bool splc_mqtt_input(const char *topic, const char *payload, size_t len, bool retained)
{
    if (topic == NULL || (payload == NULL && len > 0U)) { return false; }
    bool matched = false;

    for (unsigned b = 0U; b < BLOCKS; b++) {
        const uint16_t *r = s_run[b];
        if (!blk_enabled(r)) { continue; }
        char filter[PLC_MQTT_SUB_TOPIC_MAX + 1U];
        block_topic(r, filter);
        if (!topic_match(filter, topic)) { continue; }

        matched = true;
        s_stats.received++;
        if (retained && (r[F_FLAGS] & FLAG_IGN_RETAIN) != 0U) { s_stats.dropped++; continue; }
        if (len > PLC_MQTT_SUB_PAYLOAD_MAX)                    { s_stats.dropped++; continue; }
        enqueue(b, payload, len);
    }
    return matched;
}

/* ------------------------------------------------------------------------ */
/* Scan                                                                     */

void plc_mqtt_sub_scan(uint32_t now_ms)
{
    for (unsigned b = 0U; b < BLOCKS; b++) {
        const uint16_t *r = s_run[b];
        block_rt_t *rt = &s_rt[b];
        queue_t *q = &s_q[b];

        if (!blk_enabled(r)) { rt->armed = false; continue; }
        if (!rt->armed) {                          /* the watchdog counts from here */
            rt->armed = true;
            rt->fs_fired = false;
            rt->last_rx_ms = now_ms;
        }

        if (q->count > 0U) {                       /* one message per scan keeps their order */
            const unsigned slot = q->head;
            const bool ok = apply_message(r, q->buf[slot], q->len[slot]);
            q->head = (uint8_t)((q->head + 1U) % DEPTH);
            q->count--;
            if (ok) {
                s_stats.applied++;
                rt->last_rx_ms = now_ms;
                rt->fs_fired = false;
            } else {
                s_stats.rejected++;
            }
        }

        const uint32_t timeout = get_u32(&r[F_TIMEOUT]);
        if (timeout != 0U && !rt->fs_fired && (uint32_t)(now_ms - rt->last_rx_ms) > timeout) {
            rt->fs_fired = true;
            s_stats.failsafes++;
            apply_failsafe(r);
            log_info(TAG, "block %u: no message for %lu ms, fail-safe action %u", b, (unsigned long)timeout, blk_fs(r));
        }
    }
}

#endif /* SPLC_MQTT_SUB_ENABLE */