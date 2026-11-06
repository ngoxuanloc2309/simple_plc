/*
 * plc_zigbee.c - Zigbee port configuration (see plc_zigbee.h, SPLC-SPEC-ZIGBEE-006 v0.2).
 * Compiled only with SPLC_ZIGBEE_ENABLE = 1.
 */
#include "plc_zigbee.h"

#if SPLC_ZIGBEE_ENABLE

#include <string.h>

#include "splc_log.h"

static const char *TAG = "PLC_ZIGBEE";

#define CFG   PLC_ZIGBEE_CFG_REGS
#define REGS  PLC_ZIGBEE_REGS

/* register offsets */
#define R_CTRL     0U
#define R_SLAVE_ID 1U
#define R_PAN_ID   2U
#define R_CHANNEL  3U
#define R_KEY      5U     /* 8 regs = 16 bytes */
#define KEY_REGS   8U
#define R_LINK     32U
#define R_ERROR    33U
#define R_NODES    34U
#define R_RSSI     35U
#define R_STATUS   36U

/* CONTROL_FLAGS bits that exist (0..5); 6..15 must be 0 */
#define CTRL_KNOWN_MASK  0x003FU
/* ... and the ones that may be stored (PRESERVE_KEY is a command) */
#define CTRL_STORE_MASK  0x001FU

static uint16_t s_run[CFG];       /* RUNNING configuration; CTRL never holds PRESERVE_KEY */
static uint16_t s_draft[CFG];     /* DRAFT; the key part only holds what the host wrote */
static bool     s_draft_open;
static bool     s_key_written;    /* the draft saw a write to the key registers */
static uint32_t s_gen;

static splc_zigbee_ops_t s_ops;
static bool              s_registered;
static modbus_serial_port_t s_pipe;   /* built at register time from ops.read/write */
static bool              s_pipe_ok;

static bool is_key_reg(unsigned i) { return i >= R_KEY && i < (R_KEY + KEY_REGS); }

static bool key_nonzero(const uint16_t *regs)
{
    for (unsigned i = 0U; i < KEY_REGS; i++) {
        if (regs[R_KEY + i] != 0U) { return true; }
    }
    return false;
}

/* ------------------------------------------------------------------------ */
/* Host side                                                                */

bool splc_zigbee_register(const splc_zigbee_ops_t *ops)
{
    if (ops == NULL ||
        (ops->roles & (SPLC_ZIGBEE_ROLE_SLAVE | SPLC_ZIGBEE_ROLE_MASTER)) == 0U) {
        log_error(TAG, "register: bad ops (NULL or no role)");
        return false;
    }
    /* A Slave needs a pipe to answer on: refuse the registration instead of declaring a
     * role that cannot work (the same rule as the capability bits, decision 71). */
    if ((ops->roles & SPLC_ZIGBEE_ROLE_SLAVE) != 0U && (ops->read == NULL || ops->write == NULL)) {
        log_error(TAG, "register: role Slave needs ops.read and ops.write");
        return false;
    }
#if !(SPLC_MODBUS_SLAVE_ENABLE && (SPLC_MODBUS_LIB == SPLC_MODBUS_LIB_MB))
    /* The Slave endpoint lives in plc_mbs on the second Modbus library: without it the role
     * would be declared but never served (same rule as decision 71). */
    if ((ops->roles & SPLC_ZIGBEE_ROLE_SLAVE) != 0U) {
        log_error(TAG, "register: role Slave needs SPLC_MODBUS_SLAVE_ENABLE=1 and SPLC_MODBUS_LIB=MB");
        return false;
    }
#endif
    s_ops = *ops;
    s_registered = true;
    s_pipe_ok = (ops->read != NULL && ops->write != NULL);
    memset(&s_pipe, 0, sizeof(s_pipe));
    if (s_pipe_ok) {
        s_pipe.read     = ops->read;
        s_pipe.write    = ops->write;
        s_pipe.set_tx   = NULL;            /* transparent module: no direction pin */
        s_pipe.ctx      = ops->ctx;
        s_pipe.baudrate = ops->baudrate;
#if SPLC_MODBUS_LIB == SPLC_MODBUS_LIB_MB
        s_pipe.frame_sync = SPLC_MODBUS_SYNC_LENGTH;   /* radio: no 3.5-char silence */
#endif
    }
    log_info(TAG, "port registered, roles 0x%02X", (unsigned)(ops->roles & 0x03U));
    return true;
}

const modbus_serial_port_t *splc_zigbee_serial_port(void)
{
    return (s_registered && s_pipe_ok) ? &s_pipe : NULL;
}

bool splc_zigbee_get_config(splc_zigbee_config_t *out, uint32_t *generation)
{
    if (generation != NULL) { *generation = s_gen; }
    if (out == NULL) { return false; }
    memset(out, 0, sizeof(*out));
    const uint16_t c = s_run[R_CTRL];
    out->enable      = (c & PLC_ZIGBEE_CTRL_ENABLE) != 0U;
    out->role_slave  = (c & PLC_ZIGBEE_CTRL_ROLE_SLAVE) != 0U;
    out->security    = (c & PLC_ZIGBEE_CTRL_SECURITY) != 0U;
    out->antenna_ext = (c & PLC_ZIGBEE_CTRL_ANTENNA_EXT) != 0U;
    out->slave_id    = s_run[R_SLAVE_ID];
    out->pan_id      = s_run[R_PAN_ID];
    out->channel     = s_run[R_CHANNEL];
    for (unsigned i = 0U; i < KEY_REGS; i++) {
        out->net_key[2U * i]      = (uint8_t)(s_run[R_KEY + i] >> 8);
        out->net_key[2U * i + 1U] = (uint8_t)(s_run[R_KEY + i] & 0xFFU);
    }
    out->key_set = key_nonzero(s_run);
    return true;
}

/* ------------------------------------------------------------------------ */
/* Engine side                                                              */

void plc_zigbee_init(void)
{
    memset(s_run, 0, sizeof(s_run));
    memset(s_draft, 0, sizeof(s_draft));
    s_draft_open  = false;
    s_key_written = false;
    s_gen = 0U;
    /* s_registered / s_ops are set by the host and survive: the port may register first. */
}

uint32_t plc_zigbee_capability(void)
{
    return s_registered ? PLC_ZIGBEE_CAP_ZIGBEE : 0UL;
}

void plc_zigbee_read(uint16_t offset, uint16_t quantity, uint16_t *registers_out)
{
    splc_zigbee_status_t st;
    bool have_st = false;
    memset(&st, 0, sizeof(st));
    if ((unsigned)offset + quantity > R_LINK && s_registered && s_ops.status != NULL) {
        have_st = s_ops.status(s_ops.ctx, &st);
        if (!have_st) { memset(&st, 0, sizeof(st)); }
    }
    for (uint16_t k = 0U; k < quantity; k++) {
        const unsigned i = (unsigned)offset + k;
        uint16_t v = 0U;
        if (i < CFG) {
            v = is_key_reg(i) ? 0U : s_run[i];                  /* the key is write-only */
        } else if (i == R_LINK)   { v = st.link_state;
        } else if (i == R_ERROR)  { v = st.error_code;
        } else if (i == R_NODES)  { v = st.node_count;
        } else if (i == R_RSSI)   { v = (uint16_t)st.rssi;
        } else if (i == R_STATUS) { v = key_nonzero(s_run) ? PLC_ZIGBEE_STATUS_KEY_SET : 0U;
        }
        registers_out[k] = v;
    }
}

static void draft_open(void)
{
    if (s_draft_open) { return; }
    memcpy(s_draft, s_run, sizeof(s_draft));
    for (unsigned i = 0U; i < KEY_REGS; i++) { s_draft[R_KEY + i] = 0U; }
    s_key_written = false;
    s_draft_open  = true;
}

plc_zigbee_write_result_t plc_zigbee_write(uint16_t offset, uint16_t quantity,
                                           const uint16_t *registers)
{
    if ((uint32_t)offset + quantity > REGS) { return PLC_ZIGBEE_WRITE_BAD_ADDRESS; }
    for (uint16_t k = 0U; k < quantity; k++) {
        const unsigned i = (unsigned)offset + k;
        if (i >= CFG) { continue; }                 /* read-only part: ACK and ignore */
        draft_open();
        s_draft[i] = registers[k];
        if (is_key_reg(i)) { s_key_written = true; }
    }
    return PLC_ZIGBEE_WRITE_OK;
}

bool plc_zigbee_validate_draft(void)
{
    if (!s_draft_open) { return true; }
    const uint16_t c = s_draft[R_CTRL];

    if ((c & (uint16_t)~CTRL_KNOWN_MASK) != 0U) {
        log_warn(TAG, "COMMIT rejected: CONTROL_FLAGS bits 6..15 set (0x%04X)", (unsigned)c);
        return false;
    }
    if ((c & PLC_ZIGBEE_CTRL_FRAMING_ASCII) != 0U) {
        log_warn(TAG, "COMMIT rejected: FRAMING = ASCII is not supported (RTU only)");
        return false;
    }
    if ((c & PLC_ZIGBEE_CTRL_ENABLE) != 0U) {
        const bool slave = (c & PLC_ZIGBEE_CTRL_ROLE_SLAVE) != 0U;
        const uint8_t need = slave ? SPLC_ZIGBEE_ROLE_SLAVE : SPLC_ZIGBEE_ROLE_MASTER;
        if (!s_registered || (s_ops.roles & need) == 0U) {
            log_warn(TAG, "COMMIT rejected: enabled as %s but the port %s",
                     slave ? "Slave" : "Master",
                     s_registered ? "does not support that role" : "is not registered");
            return false;
        }
        if (slave && (s_draft[R_SLAVE_ID] < 1U || s_draft[R_SLAVE_ID] > 247U)) {
            log_warn(TAG, "COMMIT rejected: Slave ID %u outside 1..247", (unsigned)s_draft[R_SLAVE_ID]);
            return false;
        }
    }
    if ((c & PLC_ZIGBEE_CTRL_SECURITY) != 0U) {
        if ((c & PLC_ZIGBEE_CTRL_PRESERVE_KEY) != 0U) {
            if (!key_nonzero(s_run)) {
                log_warn(TAG, "COMMIT rejected: PRESERVE_KEY but no key is stored");
                return false;
            }
        } else if (!s_key_written || !key_nonzero(s_draft)) {
            log_warn(TAG, "COMMIT rejected: security on without PRESERVE_KEY needs a new non-zero key");
            return false;
        }
    }
    return true;
}

void plc_zigbee_commit_draft(void)
{
    if (!s_draft_open) { return; }
    uint16_t n[CFG];
    memcpy(n, s_draft, sizeof(n));

    const uint16_t c = s_draft[R_CTRL];
    const bool take_new_key = ((c & PLC_ZIGBEE_CTRL_SECURITY) != 0U) &&
                              ((c & PLC_ZIGBEE_CTRL_PRESERVE_KEY) == 0U);
    if (!take_new_key) {                           /* SECURITY = 0, or PRESERVE_KEY: keep the stored key */
        for (unsigned i = 0U; i < KEY_REGS; i++) { n[R_KEY + i] = s_run[R_KEY + i]; }
    }
    n[R_CTRL] = (uint16_t)(c & CTRL_STORE_MASK);   /* PRESERVE_KEY is a command, never stored */

    if (memcmp(n, s_run, sizeof(n)) != 0) {
        memcpy(s_run, n, sizeof(s_run));
        s_gen++;
        log_info(TAG, "configuration changed, generation %lu", (unsigned long)s_gen);
    }
    s_draft_open  = false;
    s_key_written = false;
}

void plc_zigbee_discard_draft(void)
{
    s_draft_open  = false;
    s_key_written = false;
}

void plc_zigbee_factory_reset(void)
{
    uint16_t zero[CFG];
    memset(zero, 0, sizeof(zero));
    if (memcmp(zero, s_run, sizeof(zero)) != 0) {
        memset(s_run, 0, sizeof(s_run));
        s_gen++;
    }
    plc_zigbee_discard_draft();
}

void plc_zigbee_export(uint8_t *out)
{
    for (unsigned i = 0U; i < CFG; i++) {
        out[2U * i]      = (uint8_t)(s_run[i] >> 8);
        out[2U * i + 1U] = (uint8_t)(s_run[i] & 0xFFU);
    }
}

bool plc_zigbee_import(const uint8_t *in)
{
    uint16_t n[CFG];
    for (unsigned i = 0U; i < CFG; i++) {
        n[i] = (uint16_t)(((uint16_t)in[2U * i] << 8) | in[2U * i + 1U]);
    }
    const uint16_t c = n[R_CTRL];
    if ((c & (uint16_t)~CTRL_STORE_MASK) != 0U || (c & PLC_ZIGBEE_CTRL_FRAMING_ASCII) != 0U) {
        log_warn(TAG, "import rejected: CONTROL_FLAGS 0x%04X", (unsigned)c);
        return false;
    }
    if ((c & PLC_ZIGBEE_CTRL_ENABLE) != 0U && (c & PLC_ZIGBEE_CTRL_ROLE_SLAVE) != 0U &&
        (n[R_SLAVE_ID] < 1U || n[R_SLAVE_ID] > 247U)) {
        log_warn(TAG, "import rejected: Slave ID %u", (unsigned)n[R_SLAVE_ID]);
        return false;
    }
    if (memcmp(n, s_run, sizeof(n)) != 0) {
        memcpy(s_run, n, sizeof(s_run));
        s_gen++;
    }
    plc_zigbee_discard_draft();
    return true;
}

#endif /* SPLC_ZIGBEE_ENABLE */