/*
 * plc_mbs.c - Layer 3. Modbus RTU Slave serving. See plc_mbs.h.
 */
#include "plc_mbs.h"

#if SPLC_MODBUS_SLAVE_ENABLE

#include <string.h>

#include "plc_tag.h"
#include "plc_mbm.h"
#include "plc_modbus_cfg.h"   /* g_device_health */
#include "splc_nmbs.h"        /* nmbs_crc_calc() only */
#include "splc_log.h"
#include "sx_time.h"
#if SPLC_REAL_ENABLE
#include "plc_tagtype.h"
#endif

static const char *TAG = "PLC_MBS";

#define PORTS      ((unsigned)SPLC_MBM_PORT_COUNT)
#define FRAME_MAX  256U      /* RTU ADU maximum */

/* Modbus exceptions */
#define EX_FUNCTION   0x01U
#define EX_ADDRESS    0x02U
#define EX_VALUE      0x03U
#define EX_BUSY       0x06U

/* ---- the fixed map ------------------------------------------------------ */
enum { R_DI = 0, R_DO, R_VFLAG, R_AI, R_VREG, R_VRET, R_CNT, R_SYS };

typedef struct {
    uint16_t start;
    uint16_t regs;
    uint8_t  kind;
    bool     ro;
} hreg_t;

static const hreg_t s_hreg[] = {
    { 0x0000U,  1U, R_DI,     true  },   /* DI_WORD (RO)                       */
    { 0x0001U,  1U, R_DO,     false },   /* DO_WORD                            */
    { 0x0002U,  2U, R_VFLAG,  false },   /* VFLAG_DWORD (high word first)      */
    { 0x0010U,  8U, R_AI,     true  },
    { 0x0020U, 64U, R_VREG,   false },
    { 0x0060U, 64U, R_VRET,   false },
    { 0x00A0U, 16U, R_CNT,    false },
    { 0x00F0U, 16U, R_SYS,    true  },
};
#define N_HREG  (sizeof(s_hreg) / sizeof(s_hreg[0]))

#define COIL_DO_START     0x0000U
#define COIL_DO_COUNT     16U
#define COIL_VFLAG_START  0x0064U
#define COIL_VFLAG_COUNT  32U
#define DI_START          0x0000U
#define DI_COUNT          16U
#define SYS_DIAG_REGS     10U

/* ---- how many tags of each group the board has ---------------------------- */
typedef struct {
    uint16_t di, dout, ai, vflag, vreg, vret, cnt;
} counts_t;

static uint16_t span(uint16_t from, uint16_t to, uint16_t cap)
{
    const uint16_t n = (to > from) ? (uint16_t)(to - from) : 0U;
    return (n > cap) ? cap : n;
}

static counts_t get_counts(void)
{
    counts_t c;
    c.di    = span(tag_di_base_index(),          tag_do_base_index(),          DI_COUNT);
    c.dout  = span(tag_do_base_index(),          tag_ai_base_index(),          COIL_DO_COUNT);
    c.ai    = span(tag_ai_base_index(),          tag_vflag_base_index(),       4U);
    c.vflag = span(tag_vflag_base_index(),       tag_vreg_base_index(),        COIL_VFLAG_COUNT);
    c.vreg  = span(tag_vreg_base_index(),        tag_vreg_retain_base_index(), 32U);
    c.vret  = span(tag_vreg_retain_base_index(), tag_counter_base_index(),     32U);
    c.cnt   = 0U;
    const uint16_t cb = tag_counter_base_index();
    while (c.cnt < 8U && (uint32_t)cb + c.cnt < MAX_TAGS && tag_get_kind((uint16_t)(cb + c.cnt)) == TAG_COUNTER) {
        c.cnt++;
    }
    return c;
}

static bool bit_of(uint16_t base, uint16_t n, uint16_t i)
{
    return i < n && tag_read((uint16_t)(base + i)) != 0;
}

static void set_bit(uint16_t base, uint16_t n, uint16_t i, bool v)
{
    if (i < n) { tag_write((uint16_t)(base + i), v ? 1 : 0); }
}

/* ---- 32-bit entries ------------------------------------------------------- */
static uint32_t float_bits(float f) { uint32_t u; memcpy(&u, &f, sizeof(u)); return u; }

static uint32_t ai_bits(uint16_t tag)
{
#if SPLC_REAL_ENABLE
    if (plc_tagtype_is_real(tag)) { return (uint32_t)tag_read(tag); }
#endif
    return float_bits((float)tag_read(tag));
}

static uint16_t sys_diag_reg(uint16_t i)
{
    const SPLC_DeviceHealth *h = &g_device_health;
    const uint32_t up = sx_get_tick_ms() / 1000U;
    switch (i) {
    case 0: return (uint16_t)(up >> 16);
    case 1: return (uint16_t)(up & 0xFFFFU);
    case 2: return h->reset_reason;
    case 3: return h->health_flags;
    case 4: return h->cpu_load_percent;
    case 5: return h->ram_usage_percent;
    case 6: return (uint16_t)(h->scan_time_ms >> 16);
    case 7: return (uint16_t)(h->scan_time_ms & 0xFFFFU);
    case 8: return (uint16_t)(h->max_scan_time_ms >> 16);
    case 9: return (uint16_t)(h->max_scan_time_ms & 0xFFFFU);
    default: return 0U;
    }
}

static const hreg_t *find_hreg(uint16_t addr, uint16_t qty)
{
    for (unsigned i = 0U; i < N_HREG; i++) {
        const hreg_t *r = &s_hreg[i];
        if (addr >= r->start && (uint32_t)addr + qty <= (uint32_t)r->start + r->regs) { return r; }
    }
    return NULL;
}

/* One holding register (absolute address inside a region found by find_hreg()). */
static uint16_t read_hreg(const hreg_t *r, uint16_t addr, const counts_t *c)
{
    const uint16_t off = (uint16_t)(addr - r->start);
    const uint16_t e = (uint16_t)(off / 2U);
    const bool hi = (off % 2U) == 0U;
    uint32_t v = 0U;
    switch (r->kind) {
    case R_DI: {
        uint16_t w = 0U;
        for (uint16_t i = 0U; i < 16U; i++) { if (bit_of(tag_di_base_index(), c->di, i)) { w |= (uint16_t)(1U << i); } }
        return w;
    }
    case R_DO: {
        uint16_t w = 0U;
        for (uint16_t i = 0U; i < 16U; i++) { if (bit_of(tag_do_base_index(), c->dout, i)) { w |= (uint16_t)(1U << i); } }
        return w;
    }
    case R_VFLAG: {
        uint16_t w = 0U;
        const uint16_t first = (off == 0U) ? 16U : 0U;     /* 0x0002 = VFLAG 16..31, 0x0003 = VFLAG 0..15 */
        for (uint16_t i = 0U; i < 16U; i++) { if (bit_of(tag_vflag_base_index(), c->vflag, (uint16_t)(first + i))) { w |= (uint16_t)(1U << i); } }
        return w;
    }
    case R_AI:   if (e < c->ai)   { v = ai_bits((uint16_t)(tag_ai_base_index() + e)); } break;
    case R_VREG: if (e < c->vreg) { v = (uint32_t)tag_read((uint16_t)(tag_vreg_base_index() + e)); } break;
    case R_VRET: if (e < c->vret) { v = (uint32_t)tag_read((uint16_t)(tag_vreg_retain_base_index() + e)); } break;
    case R_CNT:  if (e < c->cnt)  { v = (uint32_t)tag_read((uint16_t)(tag_counter_base_index() + e)); } break;
    case R_SYS:  return sys_diag_reg(off);
    default: break;
    }
    return hi ? (uint16_t)(v >> 16) : (uint16_t)(v & 0xFFFFU);
}

static void write_hreg(const hreg_t *r, uint16_t addr, uint16_t value, const counts_t *c)
{
    const uint16_t off = (uint16_t)(addr - r->start);
    const uint16_t e = (uint16_t)(off / 2U);
    const bool hi = (off % 2U) == 0U;
    uint16_t idx = 0U;
    switch (r->kind) {
    case R_DO:
        for (uint16_t i = 0U; i < 16U; i++) { set_bit(tag_do_base_index(), c->dout, i, ((value >> i) & 1U) != 0U); }
        return;
    case R_VFLAG: {
        const uint16_t first = (off == 0U) ? 16U : 0U;
        for (uint16_t i = 0U; i < 16U; i++) { set_bit(tag_vflag_base_index(), c->vflag, (uint16_t)(first + i), ((value >> i) & 1U) != 0U); }
        return;
    }
    case R_VREG: if (e >= c->vreg) { return; } idx = (uint16_t)(tag_vreg_base_index() + e); break;
    case R_VRET: if (e >= c->vret) { return; } idx = (uint16_t)(tag_vreg_retain_base_index() + e); break;
    case R_CNT:  if (e >= c->cnt)  { return; } idx = (uint16_t)(tag_counter_base_index() + e); break;
    default: return;
    }
    const uint32_t old = (uint32_t)tag_read(idx);
    const uint32_t nv = hi ? (((uint32_t)value << 16) | (old & 0xFFFFU))
                           : ((old & 0xFFFF0000UL) | value);
    tag_write(idx, (int32_t)nv);
}

/* ---- PDU execution ---------------------------------------------------------- */
static uint16_t be16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)(v & 0xFFU); }

static uint16_t exception(uint8_t *rsp, uint8_t fc, uint8_t code)
{
    rsp[0] = (uint8_t)(fc | 0x80U);
    rsp[1] = code;
    return 2U;
}

/* Which coil/discrete region holds [addr, addr+qty)? 0 = none. */
static uint8_t coil_region(uint16_t addr, uint16_t qty, bool is_di)
{
    if (is_di) {
        return ((uint32_t)addr + qty <= DI_START + DI_COUNT) ? 1U : 0U;
    }
    if ((uint32_t)addr + qty <= COIL_DO_START + COIL_DO_COUNT) { return 1U; }
    if (addr >= COIL_VFLAG_START && (uint32_t)addr + qty <= COIL_VFLAG_START + COIL_VFLAG_COUNT) { return 2U; }
    return 0U;
}

static bool coil_get(uint8_t region, bool is_di, uint16_t addr, const counts_t *c)
{
    if (is_di)         { return bit_of(tag_di_base_index(), c->di, (uint16_t)(addr - DI_START)); }
    if (region == 1U)  { return bit_of(tag_do_base_index(), c->dout, (uint16_t)(addr - COIL_DO_START)); }
    return bit_of(tag_vflag_base_index(), c->vflag, (uint16_t)(addr - COIL_VFLAG_START));
}

static void coil_set(uint8_t region, uint16_t addr, bool v, const counts_t *c)
{
    if (region == 1U) { set_bit(tag_do_base_index(), c->dout, (uint16_t)(addr - COIL_DO_START), v); }
    else              { set_bit(tag_vflag_base_index(), c->vflag, (uint16_t)(addr - COIL_VFLAG_START), v); }
}

uint16_t plc_mbs_execute(const uint8_t *req, uint16_t req_len, uint8_t *rsp, bool writes_allowed)
{
    if (req == NULL || rsp == NULL || req_len < 1U) { return 0U; }
    const uint8_t fc = req[0];
    const counts_t c = get_counts();

    switch (fc) {
    case 0x01: case 0x02: {                                   /* read coils / discrete inputs */
        const bool is_di = (fc == 0x02U);
        if (req_len != 5U) { return exception(rsp, fc, EX_VALUE); }
        const uint16_t addr = be16(&req[1]), qty = be16(&req[3]);
        if (qty < 1U || qty > 2000U) { return exception(rsp, fc, EX_VALUE); }
        const uint8_t reg = coil_region(addr, qty, is_di);
        if (reg == 0U) { return exception(rsp, fc, EX_ADDRESS); }
        const uint16_t nb = (uint16_t)((qty + 7U) / 8U);
        rsp[0] = fc; rsp[1] = (uint8_t)nb;
        memset(&rsp[2], 0, nb);
        for (uint16_t i = 0U; i < qty; i++) {
            if (coil_get(reg, is_di, (uint16_t)(addr + i), &c)) { rsp[2U + i / 8U] |= (uint8_t)(1U << (i % 8U)); }
        }
        return (uint16_t)(2U + nb);
    }
    case 0x03: {                                              /* read holding registers */
        if (req_len != 5U) { return exception(rsp, fc, EX_VALUE); }
        const uint16_t addr = be16(&req[1]), qty = be16(&req[3]);
        if (qty < 1U || qty > 125U) { return exception(rsp, fc, EX_VALUE); }
        const hreg_t *r = find_hreg(addr, qty);
        if (r == NULL) { return exception(rsp, fc, EX_ADDRESS); }
        rsp[0] = fc; rsp[1] = (uint8_t)(qty * 2U);
        for (uint16_t i = 0U; i < qty; i++) { put16(&rsp[2U + 2U * i], read_hreg(r, (uint16_t)(addr + i), &c)); }
        return (uint16_t)(2U + 2U * qty);
    }
    case 0x05: {                                              /* write single coil */
        if (req_len != 5U) { return exception(rsp, fc, EX_VALUE); }
        const uint16_t addr = be16(&req[1]), val = be16(&req[3]);
        if (val != 0xFF00U && val != 0x0000U) { return exception(rsp, fc, EX_VALUE); }
        const uint8_t reg = coil_region(addr, 1U, false);
        if (reg == 0U) { return exception(rsp, fc, EX_ADDRESS); }
        if (!writes_allowed) { return exception(rsp, fc, EX_BUSY); }
        coil_set(reg, addr, val == 0xFF00U, &c);
        memcpy(rsp, req, 5U);
        return 5U;
    }
    case 0x06: {                                              /* write single register */
        if (req_len != 5U) { return exception(rsp, fc, EX_VALUE); }
        const uint16_t addr = be16(&req[1]), val = be16(&req[3]);
        const hreg_t *r = find_hreg(addr, 1U);
        if (r == NULL) { return exception(rsp, fc, EX_ADDRESS); }
        if (r->ro) { return exception(rsp, fc, EX_VALUE); }
        if (!writes_allowed) { return exception(rsp, fc, EX_BUSY); }
        write_hreg(r, addr, val, &c);
        memcpy(rsp, req, 5U);
        return 5U;
    }
    case 0x0F: {                                              /* write multiple coils */
        if (req_len < 6U) { return exception(rsp, fc, EX_VALUE); }
        const uint16_t addr = be16(&req[1]), qty = be16(&req[3]);
        const uint16_t nb = (uint16_t)((qty + 7U) / 8U);
        if (qty < 1U || qty > 1968U || req[5] != nb || req_len != 6U + nb) { return exception(rsp, fc, EX_VALUE); }
        const uint8_t reg = coil_region(addr, qty, false);
        if (reg == 0U) { return exception(rsp, fc, EX_ADDRESS); }
        if (!writes_allowed) { return exception(rsp, fc, EX_BUSY); }
        for (uint16_t i = 0U; i < qty; i++) {
            coil_set(reg, (uint16_t)(addr + i), ((req[6U + i / 8U] >> (i % 8U)) & 1U) != 0U, &c);
        }
        memcpy(rsp, req, 5U);
        return 5U;
    }
    case 0x10: {                                              /* write multiple registers */
        if (req_len < 6U) { return exception(rsp, fc, EX_VALUE); }
        const uint16_t addr = be16(&req[1]), qty = be16(&req[3]);
        if (qty < 1U || qty > 123U || req[5] != qty * 2U || req_len != 6U + 2U * qty) { return exception(rsp, fc, EX_VALUE); }
        const hreg_t *r = find_hreg(addr, qty);
        if (r == NULL) { return exception(rsp, fc, EX_ADDRESS); }
        if (r->ro) { return exception(rsp, fc, EX_VALUE); }
        if (!writes_allowed) { return exception(rsp, fc, EX_BUSY); }
        for (uint16_t i = 0U; i < qty; i++) { write_hreg(r, (uint16_t)(addr + i), be16(&req[6U + 2U * i]), &c); }
        memcpy(rsp, req, 5U);
        return 5U;
    }
    default:
        return exception(rsp, fc, EX_FUNCTION);
    }
}

/* ---- RTU framing, per port ---------------------------------------------------- */
typedef struct {
    bool     listening;
    uint8_t  id;
    uint8_t  rx[FRAME_MAX];
    uint16_t len;
    uint32_t last_rx_ms;
} port_t;

static port_t   s_p[PORTS];
static uint8_t  s_tx[FRAME_MAX];     /* shared: one response in flight at a time */
static int      s_tx_port = -1;      /* port whose non-blocking transmit is still running */
static uint32_t s_tx_deadline_ms;
static uint32_t s_ok, s_exc, s_drop;

uint32_t plc_mbs_stat_ok(void)        { return s_ok; }
uint32_t plc_mbs_stat_exception(void) { return s_exc; }
uint32_t plc_mbs_stat_dropped(void)   { return s_drop; }

void plc_mbs_init(void)
{
    memset(s_p, 0, sizeof(s_p));
    s_tx_port = -1;
    s_ok = s_exc = s_drop = 0U;
}

static bool crc_ok(const uint8_t *f, uint16_t n)
{
    if (n < 4U) { return false; }
    const uint16_t crc = nmbs_crc_calc(f, (uint32_t)(n - 2U), NULL);
    return f[n - 2U] == (uint8_t)(crc >> 8) && f[n - 1U] == (uint8_t)(crc & 0xFFU);
}

/* Length of the request at the start of `f`: >0 total ADU length, 0 = function code with an
 * unknown length, -1 = more bytes needed to tell, -2 = impossible. */
static int expected_len(const uint8_t *f, uint16_t n)
{
    if (n < 2U) { return -1; }
    const uint8_t fc = f[1];
    if (fc >= 0x01U && fc <= 0x06U) { return 8; }
    if (fc == 0x0FU || fc == 0x10U) {
        if (n < 7U) { return -1; }
        const int total = 9 + (int)f[6];
        return (total > (int)FRAME_MAX) ? -2 : total;
    }
    return 0;
}

static void drop_front(port_t *p, uint16_t k)
{
    if (k >= p->len) { p->len = 0U; return; }
    memmove(p->rx, &p->rx[k], (size_t)(p->len - k));
    p->len = (uint16_t)(p->len - k);
}

static uint32_t idle_ms_for(unsigned port)
{
    plc_mbm_serial_t ln;
    if (!plc_mbm_port_line(port, &ln) || ln.baud == 0U) { return 5U; }
    const uint32_t bits = 1U + ln.data_bits + ((ln.parity != PLC_MBM_PARITY_NONE) ? 1U : 0U) + ln.stop_bits;
    const uint32_t ms = (uint32_t)((3500ULL * bits + ln.baud - 1U) / ln.baud);   /* 3.5 chars in ms, rounded up */
    return (ms < 2U) ? 2U : ms;
}

/* Send tx[0..len) on `port`. Blocking write: returns when the last bit is out, then drains
 * the echo. Non-blocking: remembers the transfer, tx_done() is polled by later scans. */
static void send_frame(unsigned pi, const modbus_serial_port_t *port, uint16_t len, uint32_t now_ms)
{
    plc_mbm_serial_t ln;
    const uint32_t baud = (plc_mbm_port_line(pi, &ln) && ln.baud != 0U) ? ln.baud : 9600U;
    const uint32_t frame_ms = (uint32_t)(((uint64_t)len * 12U * 1000U + baud - 1U) / baud);
    if (port->write_start != NULL && port->tx_done != NULL) {
        if (port->write_start(port->ctx, s_tx, len) == 0) {
            s_tx_port = (int)pi;
            s_tx_deadline_ms = now_ms + frame_ms + 100U;
        } else {
            s_drop++;
            log_warn(TAG, "port %u: transmit refused", pi + 1U);
        }
        return;
    }
    if (port->write == NULL) { s_drop++; return; }
    if (port->set_tx != NULL) { port->set_tx(port->ctx, true); }
    const int rc = port->write(port->ctx, s_tx, len, frame_ms + 50U);
    if (port->set_tx != NULL) { port->set_tx(port->ctx, false); }
    if (rc != 0) { s_drop++; log_warn(TAG, "port %u: transmit failed (%d)", pi + 1U, rc); }
    uint8_t scratch[32];
    int n;
    do { n = port->read(port->ctx, scratch, (uint16_t)sizeof(scratch)); } while (n > 0);   /* our own echo */
}

/* A complete, CRC-checked frame at the start of the buffer. Returns true when a response was
 * started (the caller stops for this scan: the shared transmit buffer is in use). */
static bool handle_frame(unsigned pi, port_t *p, const modbus_serial_port_t *port, uint16_t flen,
                         bool writes_allowed, uint32_t now_ms)
{
    const uint8_t unit = p->rx[0];
    const uint8_t *pdu = &p->rx[1];
    const uint16_t pdu_len = (uint16_t)(flen - 3U);
    uint8_t rsp[FRAME_MAX];

    if (unit == 0U) {                                    /* broadcast: writes only, never answered */
        const uint8_t fc = pdu[0];
        if (fc == 0x05U || fc == 0x06U || fc == 0x0FU || fc == 0x10U) {
            (void)plc_mbs_execute(pdu, pdu_len, rsp, writes_allowed);
        }
        return false;
    }
    const uint16_t rl = plc_mbs_execute(pdu, pdu_len, rsp, writes_allowed);
    if (rl == 0U) { return false; }
    if ((rsp[0] & 0x80U) != 0U) { s_exc++; } else { s_ok++; }
    s_tx[0] = unit;
    memcpy(&s_tx[1], rsp, rl);
    const uint16_t crc = nmbs_crc_calc(s_tx, (uint32_t)(1U + rl), NULL);
    s_tx[1U + rl] = (uint8_t)(crc >> 8);
    s_tx[2U + rl] = (uint8_t)(crc & 0xFFU);
    send_frame(pi, port, (uint16_t)(rl + 3U), now_ms);
    return true;
}

static void service_port(unsigned pi, uint32_t now_ms, bool suspended)
{
    port_t *p = &s_p[pi];
    const uint8_t id = plc_mbm_port_slave_id(pi);
    const modbus_serial_port_t *port = plc_mbm_port_serial_port(pi);

    if (plc_mbm_port_role(pi) != PLC_MBM_ROLE_SLAVE || id == 0U || port == NULL || port->read == NULL ||
        !plc_mbm_port_ready(pi)) {
        if (p->listening) { p->listening = false; p->len = 0U; }
        return;
    }
    if (!p->listening || p->id != id) {                  /* (re)started: nothing buffered counts */
        p->listening = true;
        p->id = id;
        p->len = 0U;
        p->last_rx_ms = now_ms;
        {   /* bytes that piled up while the port was a Master (or idle) belong to nobody */
            uint8_t scratch[32];
            int dn, guard = 64;
            do { dn = port->read(port->ctx, scratch, (uint16_t)sizeof(scratch)); } while (dn > 0 && --guard > 0);
        }
        log_info(TAG, "port %u: Slave ID %u listening", pi + 1U, (unsigned)id);
    }

    if (p->len >= FRAME_MAX) { drop_front(p, 1U); s_drop++; }
    int n = port->read(port->ctx, &p->rx[p->len], (uint16_t)(FRAME_MAX - p->len));
    if (n < 0) { n = 0; }
    if (n > 0) { p->len = (uint16_t)(p->len + (uint16_t)n); p->last_rx_ms = now_ms; }
    const bool ended = (n == 0) && p->len > 0U && (uint32_t)(now_ms - p->last_rx_ms) >= idle_ms_for(pi);

    bool started = false;
    while (p->len > 0U) {
        const uint8_t u = p->rx[0];
        if (u != id && u != 0U) { drop_front(p, 1U); continue; }      /* somebody else's bytes */
        const int need = expected_len(p->rx, p->len);
        if (need == -2) { drop_front(p, 1U); s_drop++; continue; }
        if (need == -1) {
            if (ended) { drop_front(p, 1U); continue; }
            break;
        }
        if (need == 0) {
            /* Function code with no known length: only a whole, quiet, CRC-good buffer counts.
             * An exception code (high bit) is never a request (our own echo, a slave's answer). */
            if (!ended) { break; }
            if ((p->rx[1] & 0x80U) == 0U && crc_ok(p->rx, p->len)) {
                started = handle_frame(pi, p, port, p->len, !suspended, now_ms);
                p->len = 0U;
                break;
            }
            drop_front(p, 1U);
            continue;
        }
        if (p->len < (uint16_t)need) {
            if (ended) { drop_front(p, 1U); continue; }
            break;
        }
        if (!crc_ok(p->rx, (uint16_t)need)) { drop_front(p, 1U); s_drop++; continue; }
        started = handle_frame(pi, p, port, (uint16_t)need, !suspended, now_ms);
        drop_front(p, (uint16_t)need);
        if (started) { break; }
    }
    if (ended && !started) { p->len = 0U; }
}

void plc_mbs_scan(uint32_t now_ms, bool rule_engine_suspended)
{
    if (s_tx_port >= 0) {                                /* a non-blocking answer is still going out */
        const modbus_serial_port_t *tp = plc_mbm_port_serial_port((unsigned)s_tx_port);
        if (tp == NULL || tp->tx_done == NULL || tp->tx_done(tp->ctx)) {
            s_tx_port = -1;
        } else if ((int32_t)(now_ms - s_tx_deadline_ms) >= 0) {
            if (tp->write_abort != NULL) { tp->write_abort(tp->ctx); }
            s_tx_port = -1;
            s_drop++;
            log_warn(TAG, "transmit never completed, aborted");
        } else {
            return;                                      /* shared buffer busy: serve next scan */
        }
    }
    for (unsigned i = 0U; i < PORTS; i++) {
        service_port(i, now_ms, rule_engine_suspended);
        if (s_tx_port >= 0) { break; }
    }
}

#endif /* SPLC_MODBUS_SLAVE_ENABLE */