/*
 * plc_wifi.c - Wi-Fi block 0x1300..0x13FF (see plc_wifi.h for the design).
 *
 * Wire layout (ICD SPLC-SPEC-WIFI-004, registers relative to 0x1300, Big-Endian,
 * ASCII packed two characters per register, high byte first):
 *    0        control_flags
 *    1..16    ssid[32]
 *    17..48   password[64]
 *    49..56   static ip / subnet / gateway / dns (2 regs each, High Word first)
 *    57       conn_status        RO
 *    58       rssi_dbm           RO
 *    59..60   current_ip         RO
 *    61..63   mac[6]             RO
 *    64       command            (writes act, reads 0)
 *    65       scan_state         RO
 *    66       ap_found_count     RO
 *    67       error_code         RO
 *    68..79   reserved           RO, reads 0
 *    80..255  8 x 22 regs scan results RO: ssid[32] (16), rssi (1), auth<<8|channel (1),
 *             bssid[6] (3), reserved (1)
 *
 * The 57 read/write registers are kept as the wire image itself, so a read returns
 * exactly what the App wrote (password included) and a write needs no parsing.
 */

#include "plc_wifi.h"

#if SPLC_WIFI_ENABLE

#include <string.h>
#include "sx_time.h"
#include "splc_log.h"

static const char *TAG = "PLC_WIFI";

#define OFF_CTRL            0U
#define OFF_SSID            1U
#define SSID_REGS           16U
#define OFF_PASS            17U
#define PASS_REGS           32U
#define OFF_IP              49U
#define OFF_SUBNET          51U
#define OFF_GATEWAY         53U
#define OFF_DNS             55U
#define CFG_RW_REGS         57U       /* registers 0..56 are R/W, 57..63 are the live status */
#define OFF_CONN            57U
#define OFF_RSSI            58U
#define OFF_CUR_IP          59U
#define OFF_MAC             61U
#define OFF_CMD             64U
#define OFF_SCAN_STATE      65U
#define OFF_AP_COUNT        66U
#define OFF_ERROR           67U
#define OFF_TABLE           80U
#define AP_REGS             22U

#define SAVE_PENDING_MS     60000U    /* give up waiting for GOT_IP after this long */
#define STATUS_POLL_MS      100U
#define BOOT_RETRY_MS       2000U
#define BOOT_MAX_TRIES      30U       /* ~60 s for the host's Wi-Fi to come up */

typedef enum { BOOT_LOAD = 0, BOOT_CONNECT, BOOT_DONE } boot_state_t;

static splc_wifi_ops_t s_ops;
static bool            s_registered;
static uint16_t        s_cfg[CFG_RW_REGS];
static uint16_t        s_cmd_error;
static boot_state_t    s_boot;
static uint8_t         s_boot_tries;
static uint32_t        s_boot_next_ms;
static bool            s_save_pending;
static uint32_t        s_save_deadline_ms;
static uint32_t        s_poll_next_ms;

/* ---- packing helpers ------------------------------------------------------ */

static void pack_ascii(const char *text, uint16_t *regs, uint16_t nregs)
{
    const uint16_t max = (uint16_t)(nregs * 2U);
    uint16_t len = 0U;
    while (len < max && text[len] != '\0') { len++; }
    for (uint16_t r = 0U; r < nregs; r++) {
        const uint16_t i = (uint16_t)(r * 2U);
        const uint8_t hi = (i < len)      ? (uint8_t)text[i]     : 0U;
        const uint8_t lo = (i + 1U < len) ? (uint8_t)text[i + 1] : 0U;
        regs[r] = (uint16_t)(((uint16_t)hi << 8) | lo);
    }
}

/* out must hold nregs*2 + 1 bytes; the text ends at the first NUL (or the field end). */
static void unpack_ascii(const uint16_t *regs, uint16_t nregs, char *out)
{
    uint16_t n = 0U;
    for (uint16_t r = 0U; r < nregs; r++) {
        const char hi = (char)(regs[r] >> 8);
        const char lo = (char)(regs[r] & 0xFFU);
        if (hi == '\0') { break; }
        out[n++] = hi;
        if (lo == '\0') { break; }
        out[n++] = lo;
    }
    out[n] = '\0';
}

static uint32_t reg_u32(const uint16_t *regs) { return ((uint32_t)regs[0] << 16) | regs[1]; }
static void put_u32(uint16_t *regs, uint32_t v)
{
    regs[0] = (uint16_t)(v >> 16);
    regs[1] = (uint16_t)(v & 0xFFFFU);
}

static void regs_to_config(splc_wifi_config_t *c)
{
    memset(c, 0, sizeof(*c));
    c->control_flags  = s_cfg[OFF_CTRL];
    unpack_ascii(&s_cfg[OFF_SSID], SSID_REGS, c->ssid);
    unpack_ascii(&s_cfg[OFF_PASS], PASS_REGS, c->password);
    c->static_ip      = reg_u32(&s_cfg[OFF_IP]);
    c->static_subnet  = reg_u32(&s_cfg[OFF_SUBNET]);
    c->static_gateway = reg_u32(&s_cfg[OFF_GATEWAY]);
    c->static_dns     = reg_u32(&s_cfg[OFF_DNS]);
}

static void config_to_regs(const splc_wifi_config_t *c)
{
    memset(s_cfg, 0, sizeof(s_cfg));
    s_cfg[OFF_CTRL] = c->control_flags;
    pack_ascii(c->ssid, &s_cfg[OFF_SSID], SSID_REGS);
    pack_ascii(c->password, &s_cfg[OFF_PASS], PASS_REGS);
    put_u32(&s_cfg[OFF_IP], c->static_ip);
    put_u32(&s_cfg[OFF_SUBNET], c->static_subnet);
    put_u32(&s_cfg[OFF_GATEWAY], c->static_gateway);
    put_u32(&s_cfg[OFF_DNS], c->static_dns);
}

static uint16_t sanitize_err(int err)
{
    if (err == PLC_WIFI_ERR_NONE) { return 0U; }
    if (err >= PLC_WIFI_ERR_BUSY && err <= PLC_WIFI_ERR_HW_NOT_READY) { return (uint16_t)err; }
    return (uint16_t)PLC_WIFI_ERR_DRIVER;
}

/* ---- registration --------------------------------------------------------- */

bool splc_wifi_register(const splc_wifi_ops_t *ops)
{
    if (ops == NULL || ops->connect == NULL || ops->disconnect == NULL || ops->status == NULL) {
        log_error(TAG, "register: connect, disconnect and status are required");
        return false;
    }
    s_ops = *ops;
    memset(s_cfg, 0, sizeof(s_cfg));
    s_cmd_error    = 0U;
    s_boot         = BOOT_LOAD;
    s_boot_tries   = 0U;
    s_boot_next_ms = 0U;
    s_save_pending = false;
    s_poll_next_ms = 0U;
    s_registered   = true;
    if (s_ops.save == NULL || s_ops.load == NULL) {
        log_warn(TAG, "no load/save callback: the Wi-Fi credentials will NOT survive a power cycle");
    }
    log_info(TAG, "registered (scan=%s load=%s save=%s erase=%s)",
             (s_ops.scan_start && s_ops.scan_ap) ? "yes" : "no",
             s_ops.load ? "yes" : "no", s_ops.save ? "yes" : "no", s_ops.erase ? "yes" : "no");
    return true;
}

void splc_wifi_unregister(void)
{
    s_registered = false;
    memset(&s_ops, 0, sizeof(s_ops));
    memset(s_cfg, 0, sizeof(s_cfg));
    s_save_pending = false;
}

uint32_t plc_wifi_capabilities(void) { return s_registered ? PLC_WIFI_CAP_WIFI : 0UL; }

/* ---- reads ---------------------------------------------------------------- */

static bool conn_is_up(uint16_t conn)
{
    return conn == PLC_WIFI_CONN_CONNECTED || conn == PLC_WIFI_CONN_GOT_IP ||
           conn == PLC_WIFI_CONN_AP_MODE_ACTIVE;
}

typedef struct {
    splc_wifi_status_t st;
    bool               st_valid;
    splc_wifi_ap_t     ap;
    int                ap_index;     /* index the cached `ap` belongs to, -1 none */
    bool               ap_valid;
} read_ctx_t;

static uint16_t ap_reg(read_ctx_t *rc, uint16_t table_reg)
{
    const uint16_t idx = (uint16_t)(table_reg / AP_REGS);
    const uint16_t f   = (uint16_t)(table_reg % AP_REGS);
    if (idx >= PLC_WIFI_MAX_APS || s_ops.scan_ap == NULL) { return 0U; }
    if (rc->ap_index != (int)idx) {
        memset(&rc->ap, 0, sizeof(rc->ap));
        rc->ap_valid = s_ops.scan_ap(s_ops.ctx, (uint8_t)idx, &rc->ap);
        rc->ap_index = (int)idx;
        rc->ap.ssid[sizeof(rc->ap.ssid) - 1U] = '\0';
    }
    if (!rc->ap_valid) { return 0U; }
    if (f < 16U) {                               /* ssid[32] */
        const uint16_t i = (uint16_t)(f * 2U);
        uint16_t len = 0U;
        while (len < 32U && rc->ap.ssid[len] != '\0') { len++; }
        const uint8_t hi = (i < len)      ? (uint8_t)rc->ap.ssid[i]     : 0U;
        const uint8_t lo = (i + 1U < len) ? (uint8_t)rc->ap.ssid[i + 1] : 0U;
        return (uint16_t)(((uint16_t)hi << 8) | lo);
    }
    switch (f) {
    case 16U: return (uint16_t)rc->ap.rssi_dbm;
    case 17U: return (uint16_t)(((uint16_t)rc->ap.auth_mode << 8) | rc->ap.channel);
    case 18U: return (uint16_t)(((uint16_t)rc->ap.bssid[0] << 8) | rc->ap.bssid[1]);
    case 19U: return (uint16_t)(((uint16_t)rc->ap.bssid[2] << 8) | rc->ap.bssid[3]);
    case 20U: return (uint16_t)(((uint16_t)rc->ap.bssid[4] << 8) | rc->ap.bssid[5]);
    default:  return 0U;                         /* 21 reserved */
    }
}

static uint16_t reg_value(read_ctx_t *rc, uint16_t r)
{
    if (r < CFG_RW_REGS) { return s_cfg[r]; }
    if (r >= OFF_TABLE)  { return ap_reg(rc, (uint16_t)(r - OFF_TABLE)); }
    const splc_wifi_status_t *st = rc->st_valid ? &rc->st : NULL;
    switch (r) {
    case OFF_CONN:       return st ? st->conn_status : 0U;
    case OFF_RSSI:       return (st && conn_is_up(st->conn_status)) ? (uint16_t)st->rssi_dbm
                                                                    : (uint16_t)(int16_t)-100;
    case OFF_CUR_IP:     return st ? (uint16_t)(st->ip >> 16) : 0U;
    case OFF_CUR_IP + 1: return st ? (uint16_t)(st->ip & 0xFFFFU) : 0U;
    case OFF_MAC:        return st ? (uint16_t)(((uint16_t)st->mac[0] << 8) | st->mac[1]) : 0U;
    case OFF_MAC + 1:    return st ? (uint16_t)(((uint16_t)st->mac[2] << 8) | st->mac[3]) : 0U;
    case OFF_MAC + 2:    return st ? (uint16_t)(((uint16_t)st->mac[4] << 8) | st->mac[5]) : 0U;
    case OFF_SCAN_STATE: return st ? st->scan_state : 0U;
    case OFF_AP_COUNT:   return st ? (uint16_t)((st->ap_count > PLC_WIFI_MAX_APS) ? PLC_WIFI_MAX_APS
                                                                                  : st->ap_count) : 0U;
    case OFF_ERROR:      return (st && st->error_code != 0U) ? st->error_code : s_cmd_error;
    default:             return 0U;              /* 64 command (reads IDLE), 68..79 reserved */
    }
}

void plc_wifi_read(uint16_t offset, uint16_t quantity, uint16_t *registers_out)
{
    if (!s_registered) {
        for (uint16_t i = 0U; i < quantity; i++) { registers_out[i] = 0U; }
        return;
    }
    read_ctx_t rc;
    rc.st_valid = false;
    rc.ap_index = -1;
    rc.ap_valid = false;

    /* The host is asked for its live state once per request, and only when a register
     * that needs it is part of the request. */
    const uint32_t end = (uint32_t)offset + quantity;
    const bool need_status = (offset < OFF_CMD && end > OFF_CONN) ||
                             (offset < OFF_TABLE && end > OFF_SCAN_STATE);
    if (need_status) {
        memset(&rc.st, 0, sizeof(rc.st));
        rc.st_valid = s_ops.status(s_ops.ctx, &rc.st);
    }
    for (uint16_t i = 0U; i < quantity; i++) {
        registers_out[i] = reg_value(&rc, (uint16_t)(offset + i));
    }
}

/* ---- commands and writes -------------------------------------------------- */

static void run_command(uint16_t cmd)
{
    int err = PLC_WIFI_ERR_NONE;
    switch (cmd) {
    case PLC_WIFI_CMD_START_SCAN:
        err = (s_ops.scan_start != NULL) ? s_ops.scan_start(s_ops.ctx) : PLC_WIFI_ERR_HW_NOT_READY;
        log_info(TAG, "command START_SCAN -> %d", err);
        break;
    case PLC_WIFI_CMD_CONNECT: {
        splc_wifi_config_t c;
        regs_to_config(&c);
        if (c.ssid[0] == '\0') {
            /* Not in the ICD: an empty SSID cannot be joined. Driver error (3). */
            err = PLC_WIFI_ERR_DRIVER;
            log_warn(TAG, "command CONNECT rejected: empty SSID");
        } else {
            err = s_ops.connect(s_ops.ctx, &c);
            log_info(TAG, "command CONNECT ssid_len=%u -> %d", (unsigned)strlen(c.ssid), err);
            if (err == PLC_WIFI_ERR_NONE) {
                s_save_pending     = true;       /* save once GOT_IP proves the credentials */
                s_save_deadline_ms = sx_get_tick_ms() + SAVE_PENDING_MS;
            }
        }
        memset(&c, 0, sizeof(c));                /* the password does not stay on the stack */
        break;
    }
    case PLC_WIFI_CMD_DISCONNECT:
        err = s_ops.disconnect(s_ops.ctx);
        s_save_pending = false;
        log_info(TAG, "command DISCONNECT -> %d", err);
        break;
    case PLC_WIFI_CMD_FORGET:
        (void)s_ops.disconnect(s_ops.ctx);
        if (s_ops.erase != NULL) { s_ops.erase(s_ops.ctx); }
        memset(s_cfg, 0, sizeof(s_cfg));
        s_save_pending = false;
        log_info(TAG, "command FORGET: stored credentials erased, config cleared");
        break;
    default:
        return;
    }
    s_cmd_error = sanitize_err(err);
}

plc_wifi_write_result_t plc_wifi_write(uint16_t offset, uint16_t quantity, const uint16_t *registers)
{
    if (!s_registered || quantity == 0U || offset >= OFF_TABLE ||
        (uint32_t)offset + quantity > OFF_TABLE) {
        return PLC_WIFI_WRITE_ADDR;              /* includes the read-only scan table */
    }
    const bool     has_cmd = (offset <= OFF_CMD) && (OFF_CMD < (uint32_t)offset + quantity);
    const uint16_t cmd     = has_cmd ? registers[OFF_CMD - offset] : 0U;
    if (cmd > PLC_WIFI_CMD_FORGET) {
        log_warn(TAG, "write rejected: unknown command 0x%04X", (unsigned)cmd);
        return PLC_WIFI_WRITE_VALUE;             /* nothing has been changed yet */
    }
    for (uint16_t i = 0U; i < quantity; i++) {
        const uint16_t r = (uint16_t)(offset + i);
        if (r < CFG_RW_REGS) { s_cfg[r] = registers[i]; }   /* live-status registers are ignored */
    }
    if (cmd != PLC_WIFI_CMD_IDLE) { run_command(cmd); }
    return PLC_WIFI_WRITE_OK;
}

/* ---- service: boot restore and save after GOT_IP --------------------------- */

static void boot_step(uint32_t now_ms)
{
    if (s_boot == BOOT_LOAD) {
        splc_wifi_config_t c;
        memset(&c, 0, sizeof(c));
        s_boot = BOOT_DONE;
        if (s_ops.load != NULL && s_ops.load(s_ops.ctx, &c)) {
            c.ssid[sizeof(c.ssid) - 1U] = '\0';
            c.password[sizeof(c.password) - 1U] = '\0';
            config_to_regs(&c);
            log_info(TAG, "restored stored config (ssid_len=%u flags=0x%04X)",
                     (unsigned)strlen(c.ssid), (unsigned)c.control_flags);
            if ((c.control_flags & PLC_WIFI_CTRL_ENABLE) &&
                (c.control_flags & PLC_WIFI_CTRL_AUTO_RECONNECT) && c.ssid[0] != '\0') {
                s_boot         = BOOT_CONNECT;
                s_boot_tries   = 0U;
                s_boot_next_ms = now_ms;
            }
        }
        memset(&c, 0, sizeof(c));
        return;
    }
    if (s_boot == BOOT_CONNECT && (int32_t)(now_ms - s_boot_next_ms) >= 0) {
        splc_wifi_config_t c;
        regs_to_config(&c);
        const int err = s_ops.connect(s_ops.ctx, &c);
        memset(&c, 0, sizeof(c));
        if (err == PLC_WIFI_ERR_NONE) {
            log_info(TAG, "auto-reconnect started");
            s_boot = BOOT_DONE;                  /* already stored: no save needed */
        } else if ((err == PLC_WIFI_ERR_BUSY || err == PLC_WIFI_ERR_HW_NOT_READY) &&
                   ++s_boot_tries < BOOT_MAX_TRIES) {
            s_boot_next_ms = now_ms + BOOT_RETRY_MS;     /* the host's Wi-Fi is not up yet */
        } else {
            log_warn(TAG, "auto-reconnect given up (host answered %d)", err);
            s_cmd_error = sanitize_err(err);
            s_boot = BOOT_DONE;
        }
    }
}

void plc_wifi_service(uint32_t now_ms)
{
    if (!s_registered) { return; }
    if (s_boot != BOOT_DONE) { boot_step(now_ms); }
    if (!s_save_pending || (int32_t)(now_ms - s_poll_next_ms) < 0) { return; }
    s_poll_next_ms = now_ms + STATUS_POLL_MS;

    splc_wifi_status_t st;
    memset(&st, 0, sizeof(st));
    if (s_ops.status(s_ops.ctx, &st) && st.conn_status == PLC_WIFI_CONN_GOT_IP) {
        s_save_pending = false;
        if (s_ops.save != NULL) {
            splc_wifi_config_t c;
            regs_to_config(&c);
            if (s_ops.save(s_ops.ctx, &c)) {
                log_info(TAG, "credentials saved after GOT_IP");
            } else {
                log_warn(TAG, "host failed to save the credentials");
            }
            memset(&c, 0, sizeof(c));
        }
    } else if (st.conn_status == PLC_WIFI_CONN_AUTH_FAILED ||
               st.conn_status == PLC_WIFI_CONN_SSID_NOT_FOUND) {
        s_save_pending = false;                  /* never overwrite a good record with a bad one */
        log_info(TAG, "connect failed (status %u): credentials NOT saved", (unsigned)st.conn_status);
    } else if ((int32_t)(now_ms - s_save_deadline_ms) >= 0) {
        s_save_pending = false;
        log_warn(TAG, "no GOT_IP within %u ms: credentials NOT saved", (unsigned)SAVE_PENDING_MS);
    }
}

void plc_wifi_factory_reset(void)
{
    if (!s_registered) { return; }
    if (s_ops.erase != NULL) { s_ops.erase(s_ops.ctx); }
    memset(s_cfg, 0, sizeof(s_cfg));
    s_save_pending = false;
    s_boot = BOOT_DONE;                          /* do not reload what was just erased */
    log_info(TAG, "FACTORY_RESET: stored credentials erased");
}

#endif /* SPLC_WIFI_ENABLE */