/*
 * splc_mqtt_fmt.c - Layer U. See splc_mqtt_fmt.h.
 * Integer-only, bounded, no heap, no libc string/printf functions.
 */
#include "splc_mqtt_fmt.h"

#include <float.h>

/* Compact, 8 tags: {"d":[ (6) + 8*12 + 7 commas + ],"ts": (7) + 10 + } (1) = 127 (+NUL = 128). */
_Static_assert(6U + SPLC_MQTT_FMT_MAX_TAGS * SPLC_MQTT_FMT_NUM_MAX + (SPLC_MQTT_FMT_MAX_TAGS - 1U) +
                       7U + SPLC_MQTT_FMT_TS_MAX + 1U + 1U <= 128U,
               "compact JSON must fit the smallest legal SPLC_MQTT_PAYLOAD_MAX");

/* ---------------------------------------------------------------- writer */

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;   /* counts every char, also the ones that did not fit */
    bool   ovf;
} fmt_w_t;

static void w_ch(fmt_w_t *w, char c)
{
    if ((w->len + 1U) < w->cap) {
        w->buf[w->len] = c;
    } else {
        w->ovf = true;
    }
    w->len++;
}

static void w_str(fmt_w_t *w, const char *s)
{
    while (*s != '\0') {
        w_ch(w, *s++);
    }
}

static void w_u32(fmt_w_t *w, uint32_t v)
{
    char    tmp[10];
    uint8_t n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10U));
        v /= 10U;
    } while (v != 0U);
    while (n > 0U) {
        w_ch(w, tmp[--n]);
    }
}

static void w_num(fmt_w_t *w, int32_t v, uint8_t dec)
{
    if (dec > SPLC_MQTT_FMT_DEC_MAX) {
        dec = (uint8_t)SPLC_MQTT_FMT_DEC_MAX;
    }
    /* |v| in uint32_t: also right for INT32_MIN. */
    uint32_t mag = (v < 0) ? (0U - (uint32_t)v) : (uint32_t)v;
    if (v < 0) {
        w_ch(w, '-');
    }
    uint32_t p = splc_mqtt_fmt_pow10(dec);
    w_u32(w, mag / p);
    if (dec > 0U) {
        w_ch(w, '.');
        uint32_t frac = mag % p;
        for (uint32_t d = p / 10U; d > 0U; d /= 10U) {   /* zero-pad on the left */
            w_ch(w, (char)('0' + ((frac / d) % 10U)));
        }
    }
}

static int w_done(fmt_w_t *w)
{
    if (w->cap == 0U) {
        return -1;
    }
    if (w->ovf) {
        w->buf[0] = '\0';
        return -1;
    }
    w->buf[w->len] = '\0';
    return (int)w->len;
}

static int w_fail(char *buf, size_t cap)
{
    if (cap > 0U) {
        buf[0] = '\0';
    }
    return -1;
}

static void w_init(fmt_w_t *w, char *buf, size_t cap)
{
    w->buf = buf;
    w->cap = cap;
    w->len = 0U;
    w->ovf = false;
}

/* ---------------------------------------------------------------- numbers */

uint32_t splc_mqtt_fmt_pow10(uint8_t dec)
{
    static const uint32_t tbl[4] = {1U, 10U, 100U, 1000U};
    return tbl[(dec > SPLC_MQTT_FMT_DEC_MAX) ? SPLC_MQTT_FMT_DEC_MAX : dec];
}

int splc_mqtt_fmt_number(char *buf, size_t cap, int32_t v, uint8_t dec)
{
    if (buf == NULL) {
        return -1;
    }
    fmt_w_t w;
    w_init(&w, buf, cap);
    w_num(&w, v, dec);
    return w_done(&w);
}

bool splc_mqtt_fmt_deadband_threshold(float deadband, uint8_t dec, uint32_t *out)
{
    if (out == NULL) {
        return false;
    }
    /* "!(x >= 0)" rejects NaN too; > FLT_MAX rejects +Inf. */
    if (!(deadband >= 0.0f) || (deadband > FLT_MAX)) {
        return false;
    }
    float    xf = deadband * (float)splc_mqtt_fmt_pow10(dec);
    uint32_t u;
    if (xf >= 2147483647.0f) {
        u = (uint32_t)INT32_MAX;
    } else {
        u = (uint32_t)xf;
        float frac = xf - (float)u;
        /* ~2 ulp of slack plus 1/1000 count: representation noise is not a real fraction. */
        float tol = 0.001f + (xf * 2.4e-7f);
        if (frac > tol) {
            u++;
        }
    }
    if (u == 0U) {
        u = 1U;
    }
    *out = u;
    return true;
}

#if SPLC_MQTT_FMT_REAL_ENABLE
/* ---------------------------------------------------------------- REAL (%.4g) */

/* Exact decimal expansion of a float32 (value = m * 2^e, m < 2^24):
 *   e >= 0 : digits of m * 2^e        (at most 39 digits)
 *   e <  0 : digits of m * 5^-e, value = digits * 10^e   (at most 113 digits)
 * so the rounding to 4 significant digits sees every discarded digit and ties
 * come out exactly as glibc prints them (round half to even). */
#define REAL_DIGITS_MAX 120U

static void real_mul_small(uint8_t *dg, uint8_t *len, uint8_t f)
{
    uint16_t carry = 0U;
    for (uint8_t i = 0U; i < *len; i++) {
        uint16_t t = (uint16_t)((uint16_t)dg[i] * f + carry);
        dg[i] = (uint8_t)(t % 10U);
        carry = (uint16_t)(t / 10U);
    }
    while (carry != 0U) {
        dg[(*len)++] = (uint8_t)(carry % 10U);
        carry = (uint16_t)(carry / 10U);
    }
}

static void w_real(fmt_w_t *w, uint32_t bits)
{
    const bool     neg = (bits >> 31) != 0U;
    const uint32_t ex  = (bits >> 23) & 0xFFU;
    const uint32_t fr  = bits & 0x7FFFFFU;

    if (ex == 0xFFU) {
        if (fr != 0U) {
            w_str(w, "nan");
        } else {
            w_str(w, neg ? "-inf" : "inf");
        }
        return;
    }
    if (neg) {
        w_ch(w, '-');
    }
    uint32_t m;
    int      e;
    if (ex == 0U) {
        m = fr;                 /* subnormal */
        e = -149;
    } else {
        m = fr | 0x800000UL;
        e = (int)ex - 150;
    }
    if (m == 0U) {
        w_ch(w, '0');
        return;
    }

    uint8_t dg[REAL_DIGITS_MAX];    /* least significant digit first */
    uint8_t len = 0U;
    for (uint32_t t = m; t != 0U; t /= 10U) {
        dg[len++] = (uint8_t)(t % 10U);
    }
    if (e >= 0) {
        for (int i = 0; i < e; i++) {
            real_mul_small(dg, &len, 2U);
        }
    } else {
        for (int i = 0; i < -e; i++) {
            real_mul_small(dg, &len, 5U);
        }
    }
    int d = (int)len - 1 + ((e < 0) ? e : 0);       /* decimal exponent of the leading digit */

    /* first 4 significant digits, then round on what is left */
    uint32_t n = 0U;
    for (uint8_t j = 0U; j < 4U; j++) {
        n = (n * 10U) + ((j < len) ? dg[len - 1U - j] : 0U);
    }
    if (len > 4U) {
        const uint8_t r0 = dg[len - 5U];
        bool up;
        if (r0 != 5U) {
            up = (r0 > 5U);
        } else {
            bool tail = false;
            for (uint8_t i = 0U; (len >= 6U) && (i < (uint8_t)(len - 5U)); i++) {
                if (dg[i] != 0U) {
                    tail = true;
                    break;
                }
            }
            up = tail || ((n & 1U) != 0U);
        }
        if (up) {
            n++;
            if (n == 10000U) {
                n = 1000U;
                d++;
            }
        }
    }

    uint8_t s[4] = {(uint8_t)(n / 1000U), (uint8_t)((n / 100U) % 10U),
                    (uint8_t)((n / 10U) % 10U), (uint8_t)(n % 10U)};
    uint8_t nd = 4U;
    while ((nd > 1U) && (s[nd - 1U] == 0U)) {
        nd--;
    }

    if ((d < -4) || (d >= 4)) {                     /* %e style: d.ddde+XX */
        w_ch(w, (char)('0' + s[0]));
        if (nd > 1U) {
            w_ch(w, '.');
            for (uint8_t i = 1U; i < nd; i++) {
                w_ch(w, (char)('0' + s[i]));
            }
        }
        w_ch(w, 'e');
        w_ch(w, (d < 0) ? '-' : '+');
        uint32_t ad = (uint32_t)((d < 0) ? -d : d);
        w_ch(w, (char)('0' + (ad / 10U)));
        w_ch(w, (char)('0' + (ad % 10U)));
    } else if (d >= 0) {                            /* fixed, integer part has d+1 digits */
        for (int i = 0; i <= d; i++) {
            w_ch(w, (i < (int)nd) ? (char)('0' + s[i]) : '0');
        }
        if ((int)nd > (d + 1)) {
            w_ch(w, '.');
            for (int i = d + 1; i < (int)nd; i++) {
                w_ch(w, (char)('0' + s[i]));
            }
        }
    } else {                                        /* 0.000ddd */
        w_str(w, "0.");
        for (int i = 0; i < (-d - 1); i++) {
            w_ch(w, '0');
        }
        for (uint8_t i = 0U; i < nd; i++) {
            w_ch(w, (char)('0' + s[i]));
        }
    }
}

int splc_mqtt_fmt_real(char *buf, size_t cap, uint32_t bits)
{
    if (buf == NULL) {
        return -1;
    }
    fmt_w_t w;
    w_init(&w, buf, cap);
    w_real(&w, bits);
    return w_done(&w);
}

/* One bound value: REAL and BOOL by kind, everything else int32 with dec. */
static void w_val(fmt_w_t *w, const splc_mqtt_fmt_in_t *in, uint8_t idx, int dec_override)
{
    if (in->kind[idx] == SPLC_MQTT_FMT_KIND_REAL) {
        w_real(w, (uint32_t)in->val[idx]);
    } else if (in->kind[idx] == SPLC_MQTT_FMT_KIND_BOOL) {
        w_ch(w, (in->val[idx] != 0) ? '1' : '0');
    } else {
        w_num(w, in->val[idx], (dec_override >= 0) ? (uint8_t)dec_override : in->dec[idx]);
    }
}
#define W_VAL(w, in, idx, dec_override) w_val((w), (in), (idx), (dec_override))
#else
#define W_VAL(w, in, idx, dec_override) \
    w_num((w), (in)->val[idx], ((dec_override) >= 0) ? (uint8_t)(dec_override) : (in)->dec[idx])
#endif /* SPLC_MQTT_FMT_REAL_ENABLE */

/* ---------------------------------------------------------------- compact / raw */

static bool in_valid(const splc_mqtt_fmt_in_t *in)
{
    return (in != NULL) && (in->count >= 1U) && (in->count <= SPLC_MQTT_FMT_MAX_TAGS);
}

int splc_mqtt_fmt_compact(char *buf, size_t cap, const splc_mqtt_fmt_in_t *in)
{
    if ((buf == NULL) || !in_valid(in)) {
        return -1;
    }
    fmt_w_t w;
    w_init(&w, buf, cap);
    if (in->count == 1U) {
        w_str(&w, "{\"tag\":");
        w_u32(&w, in->tag[0]);
        w_str(&w, ",\"val\":");
        W_VAL(&w, in, 0U, -1);
    } else {
        w_str(&w, "{\"d\":[");
        for (uint8_t i = 0U; i < in->count; i++) {
            if (i != 0U) {
                w_ch(&w, ',');
            }
            W_VAL(&w, in, i, -1);
        }
        w_ch(&w, ']');
    }
    w_str(&w, ",\"ts\":");
    w_u32(&w, in->ts);
    w_ch(&w, '}');
    return w_done(&w);
}

int splc_mqtt_fmt_raw(char *buf, size_t cap, const splc_mqtt_fmt_in_t *in)
{
    if ((buf == NULL) || !in_valid(in)) {
        return -1;
    }
    fmt_w_t w;
    w_init(&w, buf, cap);
    W_VAL(&w, in, 0U, -1);
    return w_done(&w);
}

/* ---------------------------------------------------------------- template parser */

typedef enum { TK_VAL, TK_TAG, TK_VALS, TK_TS, TK_DEVICE } tk_kind_t;

typedef struct {
    tk_kind_t kind;
    uint8_t   k;     /* 1..8 for VAL/TAG */
    int8_t    dec;   /* -1 = use the tag's own dec */
    size_t    len;   /* bytes consumed, from '{' through '}' */
} tok_t;

typedef enum { P_LITERAL, P_TOKEN, P_ERR_UNBAL, P_ERR_BAD, P_ERR_RANGE } p_res_t;

static bool name_is(const char *name, size_t nlen, const char *lit)
{
    size_t i = 0U;
    while (lit[i] != '\0') {
        if ((i >= nlen) || (name[i] != lit[i])) {
            return false;
        }
        i++;
    }
    return i == nlen;
}

/* Parses the digit part after "val"/"tag": "", "K" or (val only) "K:N". */
static p_res_t parse_index(const char *r, size_t rl, bool allow_dec, tok_t *t)
{
    t->k   = 1U;
    t->dec = -1;
    if (rl == 0U) {
        return P_TOKEN;
    }
    if ((r[0] < '0') || (r[0] > '9')) {
        return P_ERR_BAD;
    }
    uint8_t k = (uint8_t)(r[0] - '0');
    if ((k < 1U) || (k > SPLC_MQTT_FMT_MAX_TAGS)) {
        return P_ERR_RANGE;
    }
    t->k = k;
    if (rl == 1U) {
        return P_TOKEN;
    }
    if (allow_dec && (rl == 3U) && (r[1] == ':') && (r[2] >= '0') && (r[2] <= '3')) {
        t->dec = (int8_t)(r[2] - '0');
        return P_TOKEN;
    }
    return P_ERR_BAD;
}

/* s points at '{'; n = bytes available from s (including the '{'). */
static p_res_t parse_at(const char *s, size_t n, tok_t *t)
{
    if ((n < 2U) || (s[1] < 'a') || (s[1] > 'z')) {
        return P_LITERAL;   /* e.g. the '{' of a JSON object */
    }
    size_t j = 1U;
    while ((j < n) && (s[j] != '}')) {
        j++;
    }
    if (j >= n) {
        return P_ERR_UNBAL;
    }
    const char *name = &s[1];
    size_t      nlen = j - 1U;
    t->len = j + 1U;
    t->k   = 1U;
    t->dec = -1;

    if (nlen >= 3U && name[0] == 'v' && name[1] == 'a' && name[2] == 'l') {
        if (nlen == 4U && name[3] == 's') {
            t->kind = TK_VALS;
            return P_TOKEN;
        }
        t->kind = TK_VAL;
        return parse_index(&name[3], nlen - 3U, true, t);
    }
    if (nlen >= 3U && name[0] == 't' && name[1] == 'a' && name[2] == 'g') {
        t->kind = TK_TAG;
        return parse_index(&name[3], nlen - 3U, false, t);
    }
    if (name_is(name, nlen, "ts")) {
        t->kind = TK_TS;
        return P_TOKEN;
    }
    if (name_is(name, nlen, "device")) {
        t->kind = TK_DEVICE;
        return P_TOKEN;
    }
    return P_ERR_BAD;
}

static size_t tmpl_len(const char *tmpl, size_t tmpl_max)
{
    size_t n = 0U;
    while ((n < tmpl_max) && (tmpl[n] != '\0')) {
        n++;
    }
    return n;
}

static size_t token_worst(const tok_t *t, uint8_t count)
{
    switch (t->kind) {
    case TK_VAL:    return SPLC_MQTT_FMT_NUM_MAX;
    case TK_TAG:    return SPLC_MQTT_FMT_TAG_MAX;
    case TK_VALS:   return ((size_t)count * SPLC_MQTT_FMT_NUM_MAX) + (size_t)(count - 1U);
    case TK_TS:     return SPLC_MQTT_FMT_TS_MAX;
    case TK_DEVICE: return SPLC_MQTT_FMT_DEVICE_MAX;
    default:        return 0U;
    }
}

splc_mqtt_fmt_err_t splc_mqtt_fmt_check_template(const char *tmpl, size_t tmpl_max,
                                                 uint8_t tag_count, size_t payload_max)
{
    if ((tmpl == NULL) || (tag_count < 1U) || (tag_count > SPLC_MQTT_FMT_MAX_TAGS)) {
        return SPLC_MQTT_FMT_ERR_ARG;
    }
    size_t n     = tmpl_len(tmpl, tmpl_max);
    size_t total = 0U;
    size_t i     = 0U;
    while (i < n) {
        if (tmpl[i] != '{') {
            total++;
            i++;
            continue;
        }
        tok_t   t;
        p_res_t r = parse_at(&tmpl[i], n - i, &t);
        if (r == P_LITERAL) {
            total++;
            i++;
        } else if (r == P_TOKEN) {
            if (((t.kind == TK_VAL) || (t.kind == TK_TAG)) && (t.k > tag_count)) {
                return SPLC_MQTT_FMT_ERR_TAG_RANGE;
            }
            total += token_worst(&t, tag_count);
            i += t.len;
        } else if (r == P_ERR_UNBAL) {
            return SPLC_MQTT_FMT_ERR_UNBALANCED;
        } else if (r == P_ERR_RANGE) {
            return SPLC_MQTT_FMT_ERR_TAG_RANGE;
        } else {
            return SPLC_MQTT_FMT_ERR_BAD_TOKEN;
        }
    }
    if ((total + 1U) > payload_max) {   /* + NUL */
        return SPLC_MQTT_FMT_ERR_TOO_LONG;
    }
    return SPLC_MQTT_FMT_OK;
}

/* ---------------------------------------------------------------- custom render */

int splc_mqtt_fmt_custom(char *buf, size_t cap, const char *tmpl, size_t tmpl_max,
                         const splc_mqtt_fmt_in_t *in)
{
    if ((buf == NULL) || (tmpl == NULL) || !in_valid(in)) {
        return -1;
    }
    fmt_w_t w;
    w_init(&w, buf, cap);
    size_t n = tmpl_len(tmpl, tmpl_max);
    size_t i = 0U;
    while (i < n) {
        if (tmpl[i] != '{') {
            w_ch(&w, tmpl[i++]);
            continue;
        }
        tok_t   t;
        p_res_t r = parse_at(&tmpl[i], n - i, &t);
        if (r == P_LITERAL) {
            w_ch(&w, tmpl[i++]);
            continue;
        }
        if (r != P_TOKEN) {
            return w_fail(buf, cap);
        }
        if (((t.kind == TK_VAL) || (t.kind == TK_TAG)) && (t.k > in->count)) {
            return w_fail(buf, cap);
        }
        uint8_t idx = (uint8_t)(t.k - 1U);
        switch (t.kind) {
        case TK_VAL:
            W_VAL(&w, in, idx, t.dec);
            break;
        case TK_TAG:
            w_u32(&w, in->tag[idx]);
            break;
        case TK_VALS:
            for (uint8_t v = 0U; v < in->count; v++) {
                if (v != 0U) {
                    w_ch(&w, ',');
                }
                W_VAL(&w, in, v, -1);
            }
            break;
        case TK_TS:
            w_u32(&w, in->ts);
            break;
        case TK_DEVICE:
            if (in->device != NULL) {
                for (size_t d = 0U; (d < SPLC_MQTT_FMT_DEVICE_MAX) && (in->device[d] != '\0'); d++) {
                    w_ch(&w, in->device[d]);
                }
            }
            break;
        default:
            break;
        }
        i += t.len;
    }
    return w_done(&w);
}