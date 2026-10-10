#include "plc_io_remote.h"

#include <string.h>

#include "plc_tag.h"
#include "sx_time.h"
#include "splc_opt.h"
#include "logger.h"

static const char *TAG = "PLC_IO_REMOTE";

/* Remote I/O register block on the slave (see plc_io_remote.h). */
#define RIO_ADDR_STATUS_BLOCK  0x0C00U   /* DI, DO, STATUS */
#define RIO_ADDR_WRITE_BLOCK   0x0C03U   /* MASK, VALUE, LEASE, SAFE */

#define RIO_ST_FAILSAFE        (1U << 2)
#define RIO_ST_RESET           (1U << 3)

#define RIO_LEASE_MIN_MS       1000U
#define RIO_LEASE_MAX_MS       60000U

#define NO_TAG                 0xFFFFU

typedef enum {
    JOB_NONE = 0,
    JOB_SYNC,       /* FC16 x4: mask=all, value, lease, safe */
    JOB_WRITE,      /* FC16 x2: changed bits */
    JOB_REFRESH,    /* FC16 x2: all DO bits (lease keep-alive) */
    JOB_POLL        /* FC03 x3: DI, DO, STATUS */
} job_t;

typedef struct {
    plc_io_remote_node_cfg_t cfg;
    uint16_t di_tag[PLC_IO_REMOTE_MAX_BITS];
    uint16_t do_tag[PLC_IO_REMOTE_MAX_BITS];
    uint16_t di_reg_mask;       /* bits that have a tag */
    uint16_t do_reg_mask;
    uint16_t di_cache;          /* last DI word read from the node */
    bool     di_valid;          /* di_cache has been read at least once */
    uint16_t do_written;        /* DO bits as last confirmed by the node */
    bool     synced;            /* node holds our lease/safe config */
    bool     online;
    bool     ever_online;
    uint8_t  fail_count;
    uint32_t last_poll_ms;
    uint32_t last_write_ms;
    uint32_t retry_at_ms;       /* earliest next attempt while offline */
} node_t;

static node_t s_nodes[SPLC_REMOTE_MAX_NODES];
static int    s_node_count = 0;
static int    s_rr = 0;         /* round-robin start for the next job */

/* The one request in flight (all buses together, see output_scan). */
static struct {
    job_t                job;
    int                  node;
    uint16_t             mask;     /* what a WRITE/REFRESH/SYNC changes */
    uint16_t             value;
    modbus_rtu_master_t *master;
} s_inflight = { JOB_NONE, -1, 0, 0, NULL };

/* --- helpers ------------------------------------------------------------ */

static uint16_t bits_mask(uint8_t count)
{
    return (count >= 16U) ? 0xFFFFU : (uint16_t)((1U << count) - 1U);
}

/* Current DO word the Rule Engine wants for a node. */
static uint16_t desired_do(const node_t *n)
{
    uint16_t bits = 0U;
    for (uint16_t i = 0U; i < PLC_IO_REMOTE_MAX_BITS; i++) {
        if ((n->do_reg_mask & (1U << i)) && tag_read(n->do_tag[i]) != 0) {
            bits |= (uint16_t)(1U << i);
        }
    }
    return bits;
}

static void node_failed(int idx, const char *what)
{
    node_t *n = &s_nodes[idx];
    n->synced = false;               /* whatever happened, re-send config next */
    if (n->fail_count < 255U) {
        n->fail_count++;
    }
    if (n->fail_count == SPLC_REMOTE_OFFLINE_AFTER) {
        log_warn(TAG, "node %d (unit %u) %s after %u failed requests (%s)",
                 idx, n->cfg.unit_id, n->ever_online ? "OFFLINE" : "not answering",
                 n->fail_count, what);
    }
    if (n->fail_count >= SPLC_REMOTE_OFFLINE_AFTER) {
        n->online = false;
    }
    if (!n->online) {
        n->retry_at_ms = sx_get_tick_ms() + SPLC_REMOTE_RETRY_MS;
    }
}

static void node_answered(int idx)
{
    node_t *n = &s_nodes[idx];
    n->fail_count = 0U;
    if (!n->online) {
        n->online = true;
        log_info(TAG, "node %d (unit %u) %sONLINE", idx, n->cfg.unit_id,
                 n->ever_online ? "back " : "");
        n->ever_online = true;
    }
}

/* --- configuration ------------------------------------------------------ */

void plc_io_remote_reset(void)
{
    memset(s_nodes, 0, sizeof(s_nodes));
    s_node_count = 0;
    s_rr = 0;
    s_inflight.job = JOB_NONE;
    s_inflight.node = -1;
    s_inflight.master = NULL;
}

int plc_io_remote_add_node(const plc_io_remote_node_cfg_t *cfg)
{
    if (cfg == NULL || cfg->master == NULL ||
        cfg->unit_id < 1U || cfg->unit_id > 247U ||
        cfg->di_count > PLC_IO_REMOTE_MAX_BITS ||
        cfg->do_count > PLC_IO_REMOTE_MAX_BITS ||
        (cfg->lease_ms != 0U &&
         (cfg->lease_ms < RIO_LEASE_MIN_MS || cfg->lease_ms > RIO_LEASE_MAX_MS)) ||
        (cfg->safe_value & (uint16_t)~bits_mask(cfg->do_count))) {
        log_error(TAG, "add_node: invalid config (unit/counts/lease/safe)");
        return -1;
    }
    if (s_node_count >= SPLC_REMOTE_MAX_NODES) {
        log_error(TAG, "add_node: table full (SPLC_REMOTE_MAX_NODES=%d)",
                  SPLC_REMOTE_MAX_NODES);
        return -1;
    }
    node_t *n = &s_nodes[s_node_count];
    memset(n, 0, sizeof(*n));
    n->cfg = *cfg;
    for (uint16_t i = 0U; i < PLC_IO_REMOTE_MAX_BITS; i++) {
        n->di_tag[i] = NO_TAG;
        n->do_tag[i] = NO_TAG;
    }
    return s_node_count++;
}

static bool register_bit(uint16_t tag_idx, int node, uint8_t bit, bool is_di)
{
    if (node < 0 || node >= s_node_count || bit >= PLC_IO_REMOTE_MAX_BITS) {
        return false;
    }
    node_t   *n     = &s_nodes[node];
    uint8_t   limit = is_di ? n->cfg.di_count : n->cfg.do_count;
    uint16_t *reg   = is_di ? &n->di_reg_mask : &n->do_reg_mask;
    uint16_t *tags  = is_di ? n->di_tag : n->do_tag;

    if (bit >= limit || (*reg & (1U << bit))) {
        return false;
    }
    if (tag_get_kind(tag_idx) != (is_di ? TAG_DI : TAG_DO)) {
        return false;
    }
    /* A tag can stand for only one remote bit. */
    for (int k = 0; k < s_node_count; k++) {
        const uint16_t *other = is_di ? s_nodes[k].di_tag : s_nodes[k].do_tag;
        for (uint16_t i = 0U; i < PLC_IO_REMOTE_MAX_BITS; i++) {
            if (other[i] == tag_idx) {
                return false;
            }
        }
    }
    tags[bit] = tag_idx;
    *reg |= (uint16_t)(1U << bit);
    return true;
}

bool plc_io_remote_register_di(uint16_t tag_idx, int node, uint8_t bit)
{
    return register_bit(tag_idx, node, bit, true);
}

bool plc_io_remote_register_do(uint16_t tag_idx, int node, uint8_t bit)
{
    return register_bit(tag_idx, node, bit, false);
}

int plc_io_remote_map_node(int node, uint16_t di_tag_base, uint16_t do_tag_base)
{
    if (node < 0 || node >= s_node_count) {
        return 1;
    }
    int failed = 0;
    for (uint8_t i = 0U; i < s_nodes[node].cfg.di_count; i++) {
        if (!plc_io_remote_register_di((uint16_t)(di_tag_base + i), node, i)) {
            log_error(TAG, "node %d: map DI bit %u -> tag %u FAILED", node, i, di_tag_base + i);
            failed++;
        }
    }
    for (uint8_t i = 0U; i < s_nodes[node].cfg.do_count; i++) {
        if (!plc_io_remote_register_do((uint16_t)(do_tag_base + i), node, i)) {
            log_error(TAG, "node %d: map DO bit %u -> tag %u FAILED", node, i, do_tag_base + i);
            failed++;
        }
    }
    return failed;
}

bool plc_io_remote_node_online(int node)
{
    return node >= 0 && node < s_node_count && s_nodes[node].online;
}

int plc_io_remote_node_count(void)
{
    return s_node_count;
}

/* --- scan hooks --------------------------------------------------------- */

void plc_io_remote_input_scan(void)
{
    for (int k = 0; k < s_node_count; k++) {
        const node_t *n = &s_nodes[k];
        if (!n->di_valid) {
            continue;   /* nothing read yet: the tags keep their boot value 0 */
        }
        for (uint16_t i = 0U; i < PLC_IO_REMOTE_MAX_BITS; i++) {
            if (n->di_reg_mask & (1U << i)) {
                tag_write(n->di_tag[i], (n->di_cache >> i) & 1U);
            }
        }
    }
}

/* Starts the request for `job` on node idx. false = the master refused
 * (busy or inside the inter-frame gap); try again next cycle. */
static bool start_job(int idx, job_t job, uint16_t mask, uint16_t value, uint32_t now)
{
    node_t *n = &s_nodes[idx];
    modbus_rtu_master_t *m = n->cfg.master;
    bool started = false;

    switch (job) {
    case JOB_SYNC: {
        uint16_t regs[4] = { mask, value, n->cfg.lease_ms, n->cfg.safe_value };
        started = modbus_rtu_master_write(m, n->cfg.unit_id, RIO_ADDR_WRITE_BLOCK,
                                          regs, 4U, SPLC_REMOTE_TIMEOUT_MS);
        break;
    }
    case JOB_WRITE:
    case JOB_REFRESH: {
        uint16_t regs[2] = { mask, value };
        started = modbus_rtu_master_write(m, n->cfg.unit_id, RIO_ADDR_WRITE_BLOCK,
                                          regs, 2U, SPLC_REMOTE_TIMEOUT_MS);
        break;
    }
    case JOB_POLL:
        started = modbus_rtu_master_read(m, n->cfg.unit_id, RIO_ADDR_STATUS_BLOCK,
                                         3U, SPLC_REMOTE_TIMEOUT_MS);
        break;
    default:
        break;
    }

    if (started) {
        s_inflight.job    = job;
        s_inflight.node   = idx;
        s_inflight.mask   = mask;
        s_inflight.value  = value;
        s_inflight.master = m;
        if (job == JOB_POLL) {
            n->last_poll_ms = now;
        }
    }
    return started;
}

static void complete_job(bool ok, const uint16_t *regs)
{
    int      idx = s_inflight.node;
    node_t  *n   = &s_nodes[idx];
    job_t    job = s_inflight.job;
    uint32_t now = sx_get_tick_ms();

    s_inflight.job = JOB_NONE;

    if (!ok) {
        node_failed(idx, (job == JOB_POLL) ? "read" : "write");
        return;
    }
    node_answered(idx);

    switch (job) {
    case JOB_SYNC:
        n->synced = true;
        /* fall through */
    case JOB_WRITE:
    case JOB_REFRESH:
        n->do_written = (uint16_t)((n->do_written & ~s_inflight.mask) |
                                   (s_inflight.value & s_inflight.mask));
        n->last_write_ms = now;
        break;
    case JOB_POLL:
        n->di_cache = (uint16_t)(regs[0] & bits_mask(n->cfg.di_count));
        n->di_valid = true;
        if (regs[2] & (RIO_ST_RESET | RIO_ST_FAILSAFE)) {
            log_warn(TAG, "node %d (unit %u) status 0x%04X (reset/fail-safe): resync",
                     idx, n->cfg.unit_id, regs[2]);
            n->synced = false;
        }
        break;
    default:
        break;
    }
}

/* Picks the most urgent job over all nodes and starts it. Priority:
 * SYNC > WRITE (changed DO) > REFRESH (lease keep-alive) > POLL (DI). */
static void schedule(uint32_t now)
{
    for (int pass = 0; pass < 4; pass++) {
        for (int k = 0; k < s_node_count; k++) {
            int     idx = (s_rr + k) % s_node_count;
            node_t *n   = &s_nodes[idx];

            if (!n->online && (int32_t)(now - n->retry_at_ms) < 0) {
                continue;   /* offline and not due for another try */
            }
            uint16_t want = desired_do(n);
            uint16_t all  = n->do_reg_mask;
            bool ok = false;

            if (pass == 0) {
                if (n->synced) { continue; }
                ok = start_job(idx, JOB_SYNC, all, want, now);
            } else if (pass == 1) {
                uint16_t dirty = (uint16_t)((want ^ n->do_written) & all);
                if (!n->synced || dirty == 0U) { continue; }
                ok = start_job(idx, JOB_WRITE, dirty, want, now);
            } else if (pass == 2) {
                if (!n->synced || n->cfg.lease_ms == 0U || all == 0U ||
                    (uint32_t)(now - n->last_write_ms) < (uint32_t)(n->cfg.lease_ms / 3U)) {
                    continue;
                }
                ok = start_job(idx, JOB_REFRESH, all, want, now);
            } else {
                if (!n->synced ||
                    (uint32_t)(now - n->last_poll_ms) < SPLC_REMOTE_POLL_MS) {
                    continue;
                }
                ok = start_job(idx, JOB_POLL, 0U, 0U, now);
            }
            if (ok) {
                s_rr = (idx + 1) % s_node_count;
            }
            return;   /* one request per cycle; if the master refused, retry next cycle */
        }
    }
}

void plc_io_remote_output_scan(void)
{
    if (s_node_count == 0) {
        return;
    }
    uint32_t now = sx_get_tick_ms();

    /* collect the answer of the request in flight */
    if (s_inflight.job != JOB_NONE) {
        modbus_master_state_t st = modbus_rtu_master_poll(s_inflight.master);
        if (st == MODBUS_MASTER_DONE_OK) {
            complete_job(true, modbus_rtu_master_regs(s_inflight.master));
        } else if (st == MODBUS_MASTER_DONE_ERR) {
            log_debug(TAG, "node %d request failed: err=%d exc=%u", s_inflight.node,
                      (int)modbus_rtu_master_error(s_inflight.master),
                      (unsigned)modbus_rtu_master_exception(s_inflight.master));
            complete_job(false, NULL);
        }
    }

    /* One request in flight in total, even with several masters: simple and
     * deterministic. Revisit (per-master slots) only if two RS485 buses ever
     * need to run in parallel. */
    if (s_inflight.job == JOB_NONE) {
        schedule(now);
    }
}