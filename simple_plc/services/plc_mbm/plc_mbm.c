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

#define PORTS    ((unsigned)SPLC_MBM_PORT_COUNT)
_Static_assert(SPLC_MBM_PORT_COUNT >= 1 && SPLC_MBM_PORT_COUNT <= (int)PLC_MBM_PORT_MAX,
               "SPLC_MBM_PORT_COUNT must be 1..PLC_MBM_PORT_MAX");
/* RAM keeps the records of the ports the device HAS; the wire and the Flash image
 * always carry 4 (the rest reads as zero and is dropped on write). */
#define PT_REGS  (PORTS * PLC_MBM_PORT_REGS)

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

/* F_FLAGS: bit 0 enabled, bits 4..7 RS-485 port index (0..3) */
#define FLAG_ENABLED     0x0001U
#define FLAG_PORT_SHIFT  4U
#define FLAG_PORT_MASK   0x0FU

/* --- port record field offsets (register index inside one 16-register port) */
#define PF_CONTROL     0U   /* bit 8 enabled, bits 0..7 port number (1..4) */
#define PF_BAUD        1U   /* 2 regs */
#define PF_PARITY      3U
#define PF_FRAMING     4U   /* (data bits << 8) | stop bits */
#define PF_TIMEOUT     5U
#define PF_GAP         6U
#define PF_RETRY       7U
/* +8..+13 name (ignored). Role (SPLC-SPEC-SLAVE-001): bit 9 of +0 and +14 both say Slave
 * (+14: 0 Master, 1 Slave); +15 is the Slave ID (1..247) of a Slave port, 0 for a Master. */
#define PF_ROLE        14U
#define PF_SLAVE_ID    15U
#define PF_CTRL_ENABLED     0x0100U
#define PF_CTRL_ROLE_SLAVE  0x0200U
#define SLAVE_ID_MAX        247U

#define TAG_NONE_IDX   0xFFFFU

enum { DT_BOOL = 1, DT_INT16, DT_UINT16, DT_INT32, DT_UINT32, DT_REAL };
enum { EN_ABCD = 0, EN_CDAB, EN_BADC, EN_DCBA };
enum { ERR_NONE = 0, ERR_TIMEOUT = 1, ERR_CRC = 2, ERR_EXCEPTION = 3, ERR_MALFORMED = 4 };

#define MIN_INTERVAL_MS    PLC_SCAN_INTERVAL_MS
#define MIN_TIMEOUT_MS     10U
#define MAX_TIMEOUT_MS     60000U
#define CONFIGURE_RETRY_MS 1000U

typedef struct {
    bool     started;        /* cyclic: first poll already sent */
    bool     pending;        /* on-trigger: rising edge seen, poll not sent yet */
    bool     prev_trig;
    bool     valid_clear;    /* write valid tag back to 0 at the next scan */
    bool     retry_pending;  /* the last attempt failed and a retry is still owed */
    bool     warned;         /* the "port not usable" warning was logged */
    uint8_t  retries_left;
    uint32_t last_start_ms;
} block_rt_t;

/* One RS-485 port: the host's UART binding, the App's line settings (decoded from
 * the running port table) and the request in flight. */
typedef struct {
    modbus_rtu_master_t  master;
    bool                 registered;     /* the host gave us a usable port */
    plc_mbm_configure_fn configure;
    bool                 async_tx;       /* the port sends without blocking */
    bool                 enabled;        /* from the port table */
    plc_mbm_role_t       role;           /* from the port table: a Slave port is never a Master */
    uint8_t              slave_id;       /* its Slave ID when role is Slave, else 0 */
    plc_mbm_serial_t     serial;
    uint16_t             gap_ms;
    uint8_t              retries;
    bool                 dirty;          /* serial settings not applied to the UART yet */
    uint32_t             configure_next_ms;
    int8_t               active;         /* block whose request is in flight, -1 none; PLC_MBM_OWNER_WRITER = MB_WRITE holds the port */
    bool                 active_stale;   /* its block was re-committed: drop the answer */
#if SPLC_MBW_ENABLE
    bool                 write_wanted;   /* an MB_WRITE block could not get the port this scan: reads keep off it */
#endif
} port_rt_t;

#if SPLC_MBW_ENABLE
/* "active" value while MB_WRITE (plc_mbw) owns the port's master; it never equals a read block index. */
#define PLC_MBM_OWNER_WRITER  100
/* A port that sends with a blocking write() is started at most once per scan overall
 * (reads and writes together). plc_mbw_scan() runs BEFORE plc_mbm_scan() each scan, so the
 * flag lives in the file and is cleared at the end of plc_mbm_scan(). */
static bool s_blocking_started;
#endif

static uint16_t   s_run[PLC_MBM_BLOCKS][PLC_MBM_BLOCK_REGS];    /* RUNNING */
static uint16_t   s_draft[PLC_MBM_BLOCKS][PLC_MBM_BLOCK_REGS];  /* valid only where the mask bit is set */
static uint8_t    s_draft_mask;
static uint16_t   s_prun[PT_REGS];                              /* RUNNING port table (0x1400) */
static uint16_t   s_pdraft[PT_REGS];
static bool       s_pdraft_on;
static block_rt_t s_rt[PLC_MBM_BLOCKS];
static port_rt_t  s_port[PORTS];
static plc_mbm_role_listener_fn s_role_listener;                /* told when a port's role/ID changes */
static uint8_t    s_rr;                                         /* round-robin start */
static bool       s_boot_logged;

/* ------------------------------------------------------------------------ */

static uint32_t get_u32(const uint16_t *r) { return ((uint32_t)r[0] << 16) | r[1]; }

static bool blk_enabled(const uint16_t *b) { return (b[F_FLAGS] & FLAG_ENABLED) != 0U; }
static uint8_t blk_fc(const uint16_t *b)    { return (uint8_t)(b[F_FC_SLAVE] >> 8); }
static uint8_t blk_slave(const uint16_t *b) { return (uint8_t)(b[F_FC_SLAVE] & 0xFFU); }
static unsigned blk_port(const uint16_t *b) { return (unsigned)((b[F_FLAGS] >> FLAG_PORT_SHIFT) & FLAG_PORT_MASK); }
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

    if (blk_port(b) >= PORTS)                        { log_warn(TAG, "block %u: port %u, the device has %u", idx, blk_port(b) + 1U, PORTS); return false; }
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

/* ------------------------------------------------------------------------ */
/* Port table (0x1400..0x143F, SPLC-SPEC-MBM-003)                           */

/* What the firmware uses of one 16-register port record. Name and reserved are
 * ignored. The answer timeout of a request is the one of its MB_READ block; the
 * port's timeout field is only range-checked. */
typedef struct {
    bool             enabled;
    plc_mbm_role_t   role;
    uint8_t          slave_id;
    plc_mbm_serial_t serial;
    uint16_t         gap_ms;
    uint8_t          retries;
} port_cfg_t;

static void parse_port(const uint16_t *r, port_cfg_t *c)
{
    const uint16_t framing = r[PF_FRAMING];
    c->enabled          = (r[PF_CONTROL] & PF_CTRL_ENABLED) != 0U;
    c->role             = (((r[PF_CONTROL] & PF_CTRL_ROLE_SLAVE) != 0U) || r[PF_ROLE] == 1U) ? PLC_MBM_ROLE_SLAVE : PLC_MBM_ROLE_MASTER;
    c->slave_id         = (c->role == PLC_MBM_ROLE_SLAVE) ? (uint8_t)r[PF_SLAVE_ID] : 0U;
    c->serial.baud      = get_u32(&r[PF_BAUD]);
    c->serial.parity    = (uint8_t)r[PF_PARITY];
    c->serial.data_bits = (uint8_t)((framing >> 8) == 0U ? 8U : (framing >> 8));          /* 0 = default 8 */
    c->serial.stop_bits = (uint8_t)((framing & 0xFFU) == 0U ? 1U : (framing & 0xFFU));    /* 0 = default 1 */
    c->gap_ms           = r[PF_GAP];
    c->retries          = (uint8_t)r[PF_RETRY];
}

/* The role fields of an enabled port: bit 9 of +0 and +14 must agree (+14 is 0 or 1), and a
 * Slave needs a Slave ID of 1..247. A Master's +15 is ignored. */
static bool port_role_valid(const uint16_t *r, unsigned i)
{
    const bool bit9 = (r[PF_CONTROL] & PF_CTRL_ROLE_SLAVE) != 0U;
    if (r[PF_ROLE] > 1U) { log_warn(TAG, "port %u: role %u", i + 1U, (unsigned)r[PF_ROLE]); return false; }
    if (bit9 != (r[PF_ROLE] == 1U)) { log_warn(TAG, "port %u: role bit 9 and +14 disagree", i + 1U); return false; }
    if (bit9 && (r[PF_SLAVE_ID] < 1U || r[PF_SLAVE_ID] > SLAVE_ID_MAX)) {
        log_warn(TAG, "port %u: slave id %u", i + 1U, (unsigned)r[PF_SLAVE_ID]);
        return false;
    }
    return true;
}

/* Ports the device lacks (index >= SPLC_MBM_PORT_COUNT) are not stored or checked;
 * a disabled port is not checked either. */
static bool port_table_valid(const uint16_t *t)
{
    for (unsigned i = 0U; i < PORTS; i++) {
        const uint16_t *r = &t[i * PLC_MBM_PORT_REGS];
        if ((r[PF_CONTROL] & PF_CTRL_ENABLED) == 0U) { continue; }
        port_cfg_t c;
        parse_port(r, &c);
        if (!port_role_valid(r, i)) { return false; }
        if ((r[PF_CONTROL] & 0xFFU) != i + 1U) { log_warn(TAG, "port %u: port number %u", i + 1U, (unsigned)(r[PF_CONTROL] & 0xFFU)); return false; }
        if (c.serial.baud < 1200U || c.serial.baud > 921600U) { log_warn(TAG, "port %u: baud %lu", i + 1U, (unsigned long)c.serial.baud); return false; }
        if (c.serial.parity > PLC_MBM_PARITY_ODD) { log_warn(TAG, "port %u: parity %u", i + 1U, c.serial.parity); return false; }
        if (c.serial.data_bits != 7U && c.serial.data_bits != 8U) { log_warn(TAG, "port %u: data bits %u", i + 1U, c.serial.data_bits); return false; }
        if (c.serial.stop_bits != 1U && c.serial.stop_bits != 2U) { log_warn(TAG, "port %u: stop bits %u", i + 1U, c.serial.stop_bits); return false; }
        if (r[PF_TIMEOUT] != 0U && (r[PF_TIMEOUT] < 50U || r[PF_TIMEOUT] > 5000U)) { log_warn(TAG, "port %u: timeout %u", i + 1U, r[PF_TIMEOUT]); return false; }
        if (c.gap_ms != 0U && (c.gap_ms < 10U || c.gap_ms > 500U)) { log_warn(TAG, "port %u: inter-frame delay %u", i + 1U, c.gap_ms); return false; }
        if (c.retries > 5U) { log_warn(TAG, "port %u: retries %u", i + 1U, r[PF_RETRY]); return false; }
    }
    return true;
}

/* What a port is until the App writes 0x1400: the App's own default (9600-8-N-1,
 * 200 ms, 20 ms gap, 1 retry), enabled on every port the device has. */
static void default_port_table(uint16_t *t)
{
    memset(t, 0, PT_REGS * sizeof(uint16_t));
    for (unsigned i = 0U; i < PORTS; i++) {
        uint16_t *r = &t[i * PLC_MBM_PORT_REGS];
        r[PF_CONTROL]   = (uint16_t)(0x0100U | (i + 1U));
        r[PF_BAUD + 1U] = 9600U;
        r[PF_PARITY]    = PLC_MBM_PARITY_NONE;
        r[PF_FRAMING]   = (uint16_t)((8U << 8) | 1U);
        r[PF_TIMEOUT]   = 200U;
        r[PF_GAP]       = 20U;
        r[PF_RETRY]     = 1U;
    }
}

/* Make the (valid) running port table take effect: decode every port and mark the
 * UART of a port whose line settings changed as needing re-configuration. */
static void apply_port_table(void)
{
    for (unsigned i = 0U; i < PORTS; i++) {
        port_cfg_t c;
        parse_port(&s_prun[i * PLC_MBM_PORT_REGS], &c);
        port_rt_t *p = &s_port[i];
        const bool changed = c.serial.baud != p->serial.baud || c.serial.parity != p->serial.parity ||
                             c.serial.data_bits != p->serial.data_bits || c.serial.stop_bits != p->serial.stop_bits;
        /* A disabled port is nobody's: whatever role bits it carries, it counts as Master. */
        const plc_mbm_role_t role = c.enabled ? c.role : PLC_MBM_ROLE_MASTER;
        const uint8_t        sid  = (role == PLC_MBM_ROLE_SLAVE) ? c.slave_id : 0U;
        const bool role_changed = role != p->role || sid != p->slave_id;
        p->enabled  = c.enabled;
        p->role     = role;
        p->slave_id = sid;
        p->serial   = c.serial;
        p->gap_ms  = c.gap_ms;
        p->retries = c.retries;
        modbus_rtu_master_set_min_gap_ms(&p->master, c.gap_ms);
        if (role_changed) {
            /* Master <-> Slave: the UART is re-applied, a Master request still on the wire has
             * its answer dropped, and a write waiting for the bus is forgotten. */
            p->dirty = true;
            p->configure_next_ms = 0U;
            if (p->active >= 0) { p->active_stale = true; }
#if SPLC_MBW_ENABLE
            if (role == PLC_MBM_ROLE_SLAVE) { p->write_wanted = false; }
#endif
            if (s_role_listener != NULL) { s_role_listener(i, role, sid); }
        }
        if (changed) {
            p->dirty = true;
            p->configure_next_ms = 0U;
            if (p->active >= 0) { p->active_stale = true; }   /* the answer on the wire is for the old settings */
        }
    }
}

static void rt_reset(unsigned i)
{
    memset(&s_rt[i], 0, sizeof(s_rt[i]));
    for (unsigned p = 0U; p < PORTS; p++) {
        if (s_port[p].active == (int8_t)i) {
            s_port[p].active_stale = true;       /* the answer on the wire belongs to the old config */
        }
    }
}

static void write_status(uint16_t tag, int32_t v)
{
    if (tag != TAG_NONE_IDX) { tag_write(tag, v); }
}

/* ------------------------------------------------------------------------ */

void plc_mbm_init(void)
{
    for (unsigned p = 0U; p < PORTS; p++) {
        s_port[p].active = -1;                   /* registration done by the host is kept */
        s_port[p].active_stale = false;
    }
    plc_mbm_clear_all();
    for (unsigned p = 0U; p < PORTS; p++) {      /* first apply: whatever the table says */
        s_port[p].dirty = true;
        s_port[p].configure_next_ms = 0U;
    }
    s_boot_logged = false;
#if SPLC_MBW_ENABLE
    s_blocking_started = false;
#endif
}

void plc_mbm_clear_all(void)
{
    memset(s_run, 0, sizeof(s_run));
    memset(s_draft, 0, sizeof(s_draft));
    s_draft_mask = 0U;
    s_pdraft_on  = false;
    default_port_table(s_prun);
    for (unsigned i = 0U; i < PLC_MBM_BLOCKS; i++) { rt_reset(i); }
    apply_port_table();
}

uint16_t plc_mbm_port_count(void) { return (uint16_t)PORTS; }

plc_mbm_role_t plc_mbm_port_role(unsigned idx)
{
    return (idx < PORTS && s_port[idx].enabled) ? s_port[idx].role : PLC_MBM_ROLE_MASTER;
}

uint8_t plc_mbm_port_slave_id(unsigned idx)
{
    return (idx < PORTS && s_port[idx].enabled && s_port[idx].role == PLC_MBM_ROLE_SLAVE) ? s_port[idx].slave_id : 0U;
}

uint16_t plc_mbm_port_response_delay_ms(unsigned idx)
{
    return (idx < PORTS && s_port[idx].enabled && s_port[idx].role == PLC_MBM_ROLE_SLAVE) ? s_port[idx].gap_ms : 0U;
}

bool plc_mbm_port_is_slave_after_commit(unsigned idx)
{
    if (idx >= PORTS) { return false; }
    const uint16_t *t = s_pdraft_on ? s_pdraft : s_prun;
    port_cfg_t c;
    parse_port(&t[idx * PLC_MBM_PORT_REGS], &c);
    return c.enabled && c.role == PLC_MBM_ROLE_SLAVE;
}

void plc_mbm_set_role_listener(plc_mbm_role_listener_fn fn) { s_role_listener = fn; }

void splc_mbm_set_rs485_port(uint8_t port_index, const modbus_serial_port_t *port, plc_mbm_configure_fn configure)
{
    if (port_index >= PORTS) {
        log_warn(TAG, "RS485 port %u ignored: the device is built for %u port(s) (SPLC_MBM_PORT_COUNT)",
                 (unsigned)port_index + 1U, PORTS);
        return;
    }
    port_rt_t *p = &s_port[port_index];
    p->registered = (port != NULL) && modbus_rtu_master_init(&p->master, port);
    p->configure  = configure;
    p->async_tx   = p->registered && port->write_start != NULL && port->tx_done != NULL;
    p->dirty      = true;
    p->configure_next_ms = 0U;
    p->active     = -1;
    p->active_stale = false;
    if (p->registered) { modbus_rtu_master_set_min_gap_ms(&p->master, p->gap_ms); }
    log_info(TAG, "RS485 port %u %s, %s TX, configure callback %s",
             (unsigned)port_index + 1U, p->registered ? "registered" : "NOT usable",
             p->async_tx ? "non-blocking" : "blocking",
             (configure != NULL) ? "present" : "absent (line speed = port baudrate)");
}

void plc_mbm_read(uint16_t offset, uint16_t quantity, uint16_t *registers_out)
{
    for (uint16_t i = 0U; i < quantity; i++) {
        const uint16_t o = (uint16_t)(offset + i);
        registers_out[i] = (o < PLC_MBM_REGS) ? s_run[o / PLC_MBM_BLOCK_REGS][o % PLC_MBM_BLOCK_REGS] : 0U;
    }
}

plc_mbm_write_result_t plc_mbm_write(uint16_t offset, uint16_t quantity, const uint16_t *registers)
{
    if ((uint32_t)offset + quantity > PLC_MBM_REGS) { return PLC_MBM_WRITE_BAD_ADDRESS; }
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

void plc_mbm_ports_read(uint16_t offset, uint16_t quantity, uint16_t *registers_out)
{
    for (uint16_t i = 0U; i < quantity; i++) {
        const uint16_t o = (uint16_t)(offset + i);
        registers_out[i] = (o < PT_REGS) ? s_prun[o] : 0U;
    }
}

plc_mbm_write_result_t plc_mbm_ports_write(uint16_t offset, uint16_t quantity, const uint16_t *registers)
{
    if ((uint32_t)offset + quantity > PLC_MBM_PORT_TABLE_REGS) { return PLC_MBM_WRITE_BAD_ADDRESS; }
    if (!s_pdraft_on) {
        memcpy(s_pdraft, s_prun, sizeof(s_pdraft));    /* a partial write keeps the rest */
        s_pdraft_on = true;
    }
    for (uint16_t i = 0U; i < quantity; i++) {
        if ((unsigned)offset + i < PT_REGS) { s_pdraft[offset + i] = registers[i]; }   /* ports the device lacks are dropped */
    }
    return PLC_MBM_WRITE_OK;
}

bool plc_mbm_validate_draft(void)
{
    for (unsigned i = 0U; i < PLC_MBM_BLOCKS; i++) {
        if ((s_draft_mask & (1U << i)) != 0U && !block_valid(s_draft[i], i)) { return false; }
    }
    if (s_pdraft_on && !port_table_valid(s_pdraft)) { return false; }
    /* A block that will be enabled must not name an enabled Slave port (checked on the
     * blocks and the port table as they will be after this COMMIT). */
    for (unsigned i = 0U; i < PLC_MBM_BLOCKS; i++) {
        const uint16_t *b = ((s_draft_mask & (1U << i)) != 0U) ? s_draft[i] : s_run[i];
        if (!blk_enabled(b) || blk_port(b) >= PORTS) { continue; }
        if (plc_mbm_port_is_slave_after_commit(blk_port(b))) {
            log_warn(TAG, "block %u uses port %u, which is a Modbus Slave", i, blk_port(b) + 1U);
            return false;
        }
    }
    return true;
}

void plc_mbm_commit_draft(void)
{
    unsigned enabled = 0U;
    if (s_pdraft_on) {
        memcpy(s_prun, s_pdraft, sizeof(s_prun));
        apply_port_table();
        log_info(TAG, "COMMIT: port table");
    }
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
    s_pdraft_on  = false;
}

void plc_mbm_discard_draft(void) { s_draft_mask = 0U; s_pdraft_on = false; }

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

void plc_mbm_export(uint8_t *out)
{
    for (unsigned b = 0U; b < PLC_MBM_BLOCKS; b++) {
        put_regs(&out[b * PLC_MBM_BLOCK_REGS * 2U], s_run[b], PLC_MBM_BLOCK_REGS);
    }
    put_regs(&out[PLC_MBM_REGS * 2U], s_prun, PT_REGS);
    memset(&out[(PLC_MBM_REGS + PT_REGS) * 2U], 0, (PLC_MBM_PORT_TABLE_REGS - PT_REGS) * 2U);
}

bool plc_mbm_import(const uint8_t *in)
{
    uint16_t blk[PLC_MBM_BLOCK_REGS];   /* 128 B on the stack: no 640 B static copy of the image */
    for (unsigned i = 0U; i < PLC_MBM_BLOCKS; i++) {
        get_regs(&in[i * PLC_MBM_BLOCK_REGS * 2U], blk, PLC_MBM_BLOCK_REGS);
        if (!block_valid(blk, i)) { return false; }              /* nothing applied yet */
    }
    uint16_t ports[PT_REGS];
    get_regs(&in[PLC_MBM_REGS * 2U], ports, PT_REGS);
    if (!port_table_valid(ports)) { return false; }
    for (unsigned i = 0U; i < PLC_MBM_BLOCKS; i++) {
        get_regs(&in[i * PLC_MBM_BLOCK_REGS * 2U], s_run[i], PLC_MBM_BLOCK_REGS);
        rt_reset(i);
    }
    memcpy(s_prun, ports, sizeof(s_prun));
    apply_port_table();
    s_draft_mask = 0U;
    s_pdraft_on  = false;
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

static void finish_ok(unsigned blk, const modbus_rtu_master_t *m)
{
    const uint16_t *b = s_run[blk];
    const uint16_t *r = modbus_rtu_master_regs(m);
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

static void finish_err(unsigned blk, const modbus_rtu_master_t *m)
{
    const uint16_t *b = s_run[blk];
    int32_t code;
    switch (modbus_rtu_master_error(m)) {
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

/* Apply the App's line settings to the UART of one port. Only while the port is
 * idle: re-programming the UART under a frame that is going out would destroy it. */
static void apply_serial(port_rt_t *p, unsigned idx, uint32_t now_ms)
{
    if (!p->dirty || (int32_t)(now_ms - p->configure_next_ms) < 0 || modbus_rtu_master_is_busy(&p->master)) {
        return;
    }
    if (p->configure == NULL) {
        p->dirty = false;             /* nothing to apply: the port keeps its own line speed */
        log_info(TAG, "port %u: no configure callback, line settings stay as the host set them (baud %lu)",
                 idx + 1U, (unsigned long)p->master.port.baudrate);
        return;
    }
    if (p->configure(p->master.port.ctx, &p->serial) == 0) {
        p->master.port.baudrate = p->serial.baud;   /* only feeds the inter-frame gap / TX timeout */
        p->dirty = false;
        log_info(TAG, "port %u: %lu baud, %u data bits, parity %u, stop %u", idx + 1U,
                 (unsigned long)p->serial.baud, (unsigned)p->serial.data_bits,
                 (unsigned)p->serial.parity, (unsigned)p->serial.stop_bits);
    } else {
        p->configure_next_ms = now_ms + CONFIGURE_RETRY_MS;
        log_warn(TAG, "port %u: host could not apply %lu baud / %u bits / parity %u / stop %u, retrying", idx + 1U,
                 (unsigned long)p->serial.baud, (unsigned)p->serial.data_bits,
                 (unsigned)p->serial.parity, (unsigned)p->serial.stop_bits);
    }
}

static bool block_due(unsigned i, uint32_t now_ms)
{
    const uint16_t *b = s_run[i];
    if (!blk_enabled(b)) { return false; }
    if (s_rt[i].retry_pending) { return true; }
    if (b[F_READ_MODE] == 1U) { return s_rt[i].pending; }
    return !s_rt[i].started ||
           (uint32_t)(now_ms - s_rt[i].last_start_ms) >= get_u32(&b[F_INTERVAL]);
}

/* A due block whose port cannot be used (not registered, or disabled by the App):
 * the poll is not sent and counts as a timeout, once per interval. */
static void fail_block_no_port(unsigned i, uint32_t now_ms, const char *why)
{
    const uint16_t *b = s_run[i];
    if (!s_rt[i].warned) {
        s_rt[i].warned = true;
        log_warn(TAG, "block %u: port %u %s, the block reports err_code 1", i, blk_port(b) + 1U, why);
    }
    write_status(b[F_VALID_TAG], 0);
    write_status(b[F_ERROR_TAG], 1);
    write_status(b[F_ERRCODE_TAG], ERR_TIMEOUT);
    s_rt[i].valid_clear   = false;
    s_rt[i].started       = true;
    s_rt[i].pending       = false;
    s_rt[i].retry_pending = false;
    s_rt[i].last_start_ms = now_ms;
}

/* The request of port `p` finished: hand the answer to its block. */
static void port_result(port_rt_t *p, modbus_master_state_t st)
{
    const unsigned blk   = (unsigned)p->active;
    const bool     stale = p->active_stale;
    p->active = -1;
    p->active_stale = false;
    if (stale) { return; }

    if (st == MODBUS_MASTER_DONE_OK) {
        s_rt[blk].retry_pending = false;
        finish_ok(blk, &p->master);
        return;
    }
    /* A slave exception is an answer: do not repeat it. Timeouts, CRC errors and
     * broken frames are retried as often as the port was configured to. */
    if (modbus_rtu_master_error(&p->master) != MODBUS_MASTER_ERR_EXCEPTION && s_rt[blk].retries_left > 0U) {
        s_rt[blk].retries_left--;
        s_rt[blk].retry_pending = true;
        return;
    }
    s_rt[blk].retry_pending = false;
    finish_err(blk, &p->master);
}

void plc_mbm_scan(uint32_t now_ms)
{
    if (!s_boot_logged) {
        s_boot_logged = true;
        for (unsigned p = 0U; p < PORTS; p++) {
            if (!s_port[p].registered) {
                log_warn(TAG, "port %u of %u has no RS485 port registered: blocks on it report err_code 1", p + 1U, PORTS);
            }
        }
    }
    for (unsigned p = 0U; p < PORTS; p++) {
        if (s_port[p].registered) { apply_serial(&s_port[p], p, now_ms); }
    }

    /* The valid tag is a one-scan pulse. */
    for (unsigned i = 0U; i < PLC_MBM_BLOCKS; i++) {
        if (s_rt[i].valid_clear) {
            write_status(s_run[i][F_VALID_TAG], 0);
            s_rt[i].valid_clear = false;
        }
    }

    /* Rising edge of every on-trigger block's tag, even while a bus is busy. */
    for (unsigned i = 0U; i < PLC_MBM_BLOCKS; i++) {
        const uint16_t *b = s_run[i];
        if (blk_enabled(b) && b[F_READ_MODE] == 1U) {
            const bool now = tag_read(b[F_TRIG_TAG]) != 0;
            if (now && !s_rt[i].prev_trig) { s_rt[i].pending = true; }
            s_rt[i].prev_trig = now;
        }
    }

    /* Collect the answer of the request in flight on every port. */
    for (unsigned p = 0U; p < PORTS; p++) {
        port_rt_t *pr = &s_port[p];
        if (pr->active < 0) { continue; }
#if SPLC_MBW_ENABLE
        if (pr->active >= PLC_MBM_OWNER_WRITER) { continue; }   /* MB_WRITE polls its own request */
#endif
        const modbus_master_state_t st = modbus_rtu_master_poll(&pr->master);
        if (st != MODBUS_MASTER_BUSY) { port_result(pr, st); }
    }

    /* Start requests: at most one per port (one request in flight per bus). A port
     * whose write blocks (no non-blocking TX) is started at most once per scan
     * overall, so the scan is never held for more than one frame. */
#if SPLC_MBW_ENABLE
#define BLOCKING_STARTED  s_blocking_started
#else
    bool blocking_started = false;
#define BLOCKING_STARTED  blocking_started
#endif
    for (unsigned n = 0U; n < PLC_MBM_BLOCKS; n++) {
        const unsigned i = (s_rr + n) % PLC_MBM_BLOCKS;
        if (!block_due(i, now_ms)) { continue; }
        const uint16_t *b = s_run[i];
        port_rt_t *pr = &s_port[blk_port(b)];     /* validated: < PORTS */

        if (!pr->registered) { fail_block_no_port(i, now_ms, "has no RS485 port registered"); continue; }
        if (!pr->enabled)    { fail_block_no_port(i, now_ms, "is disabled in the port table"); continue; }
        if (pr->role == PLC_MBM_ROLE_SLAVE) { fail_block_no_port(i, now_ms, "is on a port configured as Modbus Slave"); continue; }
        if (pr->dirty && pr->configure != NULL) { continue; }   /* do not poll with the wrong line settings */
        if (pr->active >= 0)                    { continue; }   /* this bus is busy */
#if SPLC_MBW_ENABLE
        if (pr->write_wanted)                   { continue; }   /* a write is waiting for this bus: it goes first */
#endif
        if (!pr->async_tx && BLOCKING_STARTED)  { continue; }

        if (modbus_rtu_master_read_fc(&pr->master, blk_slave(b), blk_fc(b), b[F_START], b[F_COUNT],
                                      get_u32(&b[F_TIMEOUT]))) {
            if (!s_rt[i].retry_pending) {
                s_rt[i].retries_left  = pr->retries;
                s_rt[i].last_start_ms = now_ms;
            }
            s_rt[i].retry_pending = false;
            s_rt[i].started = true;
            s_rt[i].pending = false;
            pr->active = (int8_t)i;
            pr->active_stale = false;
            s_rr = (uint8_t)((i + 1U) % PLC_MBM_BLOCKS);
            if (!pr->async_tx) { BLOCKING_STARTED = true; }
        }
        /* false: the inter-frame gap has not elapsed (retry next scan) -- nothing consumed */
    }
#if SPLC_MBW_ENABLE
    s_blocking_started = false;
    for (unsigned p = 0U; p < PORTS; p++) { s_port[p].write_wanted = false; }
#endif
}

#if SPLC_MBW_ENABLE
plc_mbm_claim_t plc_mbm_port_claim(unsigned idx, modbus_rtu_master_t **master, uint8_t *retries)
{
    if (idx >= PORTS) { return PLC_MBM_CLAIM_UNUSABLE; }
    port_rt_t *p = &s_port[idx];
    if (!p->registered || !p->enabled || p->role == PLC_MBM_ROLE_SLAVE) { return PLC_MBM_CLAIM_UNUSABLE; }
    if ((p->dirty && p->configure != NULL) || p->active >= 0 || (!p->async_tx && s_blocking_started)) {
        p->write_wanted = true;
        return PLC_MBM_CLAIM_BUSY;
    }
    p->active       = PLC_MBM_OWNER_WRITER;
    p->active_stale = false;
    if (!p->async_tx) { s_blocking_started = true; }
    *master  = &p->master;
    *retries = p->retries;
    return PLC_MBM_CLAIM_OK;
}

void plc_mbm_port_release(unsigned idx, bool sent)
{
    if (idx >= PORTS) { return; }
    port_rt_t *p = &s_port[idx];
    if (p->active != PLC_MBM_OWNER_WRITER) { return; }
    p->active        = -1;
    p->active_stale  = false;
    if (!sent && !p->async_tx) { s_blocking_started = false; }   /* nothing went out: give the slot back */
}
#endif

#endif /* SPLC_MBM_ENABLE */