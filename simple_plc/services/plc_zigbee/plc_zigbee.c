/*
 * plc_zigbee.c - Zigbee port configuration (see plc_zigbee.h, SPLC-SPEC-ZIGBEE-006-ICD v2.0.0).
 * Compiled only with SPLC_ZIGBEE_ENABLE = 1.
 */
#include "plc_zigbee.h"

#if SPLC_ZIGBEE_ENABLE

#include <string.h>

#include "splc_log.h"

static const char *TAG = "PLC_ZIGBEE";

#define CFG   PLC_ZIGBEE_CFG_REGS
#define REGS  PLC_ZIGBEE_REGS

/* register offsets (ICD section 3) */
#define R_CTRL       0U
#define R_CHANNEL    1U
#define R_PAN_ID     2U
#define R_LOCAL      3U
#define R_DEST       4U
#define R_POWER      5U
#define R_BAUD       6U
#define R_PARITY     7U
#define R_DATA_BITS  8U
#define R_STOP_BITS  9U
#define R_RESP_TO    10U
#define R_IFD        11U
#define R_RETRY      12U
#define R_SLAVE_ID   13U
#define R_KEY        16U    /* 8 regs = 16 bytes */
#define KEY_REGS     8U
#define R_NAME       24U    /* 8 regs = 16 bytes */
#define NAME_REGS    8U
#define R_STATUS     36U
#define R_LINK       37U
#define R_RSSI       38U
#define R_LQI        39U
#define R_TX_PKT     40U
#define R_RX_PKT     41U
#define R_TX_ERR     42U
#define R_RX_ERR     43U
#define R_ERROR      44U

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
    /* What THIS BUILD can serve: the Slave endpoint lives in plc_mbs and needs the second Modbus
     * library (SPLC_MODBUS_LIB = MB) plus SPLC_MODBUS_SLAVE_ENABLE. A role the build cannot serve
     * is dropped from the registration (logged), so the capability bits stay honest. */
    uint8_t roles = (uint8_t)(ops->roles & (SPLC_ZIGBEE_ROLE_SLAVE | SPLC_ZIGBEE_ROLE_MASTER));
#if !(SPLC_MODBUS_SLAVE_ENABLE && SPLC_MODBUS_LIB == SPLC_MODBUS_LIB_MB)
    if ((roles & SPLC_ZIGBEE_ROLE_SLAVE) != 0U) {
        log_warn(TAG, "register: Slave role ignored (needs SPLC_MODBUS_SLAVE_ENABLE=1 and SPLC_MODBUS_LIB=MB)");
        roles &= (uint8_t)~SPLC_ZIGBEE_ROLE_SLAVE;
    }
#endif
    if (roles == 0U) {
        log_error(TAG, "register: no role this build can serve");
        return false;
    }
    /* A Slave needs a pipe to answer on: refuse the registration instead of declaring a
     * role that cannot work (the same rule as the capability bits, decision 71). */
    if ((roles & SPLC_ZIGBEE_ROLE_SLAVE) != 0U && (ops->read == NULL || ops->write == NULL)) {
        log_error(TAG, "register: role Slave needs ops.read and ops.write");
        return false;
    }
    /* A Master role without a pipe stays registrable (a port may only configure the module), but
     * MB_READ / MB_WRITE blocks on Zigbee then report err_code 1: see plc_zigbee_master_supported(). */
    s_ops = *ops;
    s_ops.roles = roles;           /* what the build serves, not what the port asked for */
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
    log_info(TAG, "port registered, roles 0x%02X", (unsigned)(roles & 0x03U));
    return true;
}

const modbus_serial_port_t *splc_zigbee_serial_port(void)
{
    return (s_registered && s_pipe_ok) ? &s_pipe : NULL;
}

bool plc_zigbee_master_supported(void)
{
    return s_registered && s_pipe_ok && (s_ops.roles & SPLC_ZIGBEE_ROLE_MASTER) != 0U;
}

bool plc_zigbee_is_slave_after_commit(void)
{
    const uint16_t c = s_draft_open ? s_draft[R_CTRL] : s_run[R_CTRL];
    return (c & PLC_ZIGBEE_CTRL_ENABLE) != 0U && (c & PLC_ZIGBEE_CTRL_ROLE_SLAVE) != 0U;
}

/* Decode 32 configuration registers (running, or the result of a COMMIT) for the port. */
static void decode_config(const uint16_t *r, splc_zigbee_config_t *out)
{
    memset(out, 0, sizeof(*out));
    const uint16_t c = r[R_CTRL];
    out->enable      = (c & PLC_ZIGBEE_CTRL_ENABLE) != 0U;
    out->role_slave  = (c & PLC_ZIGBEE_CTRL_ROLE_SLAVE) != 0U;
    out->security    = (c & PLC_ZIGBEE_CTRL_SECURITY) != 0U;
    out->antenna_ext = (c & PLC_ZIGBEE_CTRL_ANTENNA_EXT) != 0U;
    out->channel              = r[R_CHANNEL];
    out->pan_id               = r[R_PAN_ID];
    out->local_addr           = r[R_LOCAL];
    out->dest_addr            = r[R_DEST];
    out->power_level          = r[R_POWER];
    out->baud_rate_idx        = r[R_BAUD];
    out->parity               = r[R_PARITY];
    out->data_bits            = r[R_DATA_BITS];
    out->stop_bits            = r[R_STOP_BITS];
    out->response_timeout_ms  = r[R_RESP_TO];
    out->inter_frame_delay_ms = r[R_IFD];
    out->retry_count          = r[R_RETRY];
    out->slave_id             = r[R_SLAVE_ID];
    for (unsigned i = 0U; i < NAME_REGS; i++) {
        out->name[2U * i]      = (char)(r[R_NAME + i] >> 8);
        out->name[2U * i + 1U] = (char)(r[R_NAME + i] & 0xFFU);
    }
    out->name[PLC_ZIGBEE_NAME_BYTES - 1U] = '\0';   /* a consumer can always treat it as a string */
    for (unsigned i = 0U; i < KEY_REGS; i++) {
        out->net_key[2U * i]      = (uint8_t)(r[R_KEY + i] >> 8);
        out->net_key[2U * i + 1U] = (uint8_t)(r[R_KEY + i] & 0xFFU);
    }
    out->key_set = key_nonzero(r);
}

/* What is wrong with the RF / UART / name / ID fields of an ENABLED block (NULL = fine). One
 * function judges a COMMIT draft and a Flash image. A disabled block is never judged. */
static const char *enabled_fields_error(const uint16_t *r)
{
    const bool slave = (r[R_CTRL] & PLC_ZIGBEE_CTRL_ROLE_SLAVE) != 0U;
    if (r[R_CHANNEL] < 11U || r[R_CHANNEL] > 26U)             { return "CHANNEL outside 11..26"; }
    if (r[R_PAN_ID] < 0x0001U || r[R_PAN_ID] > 0xFFFEU)       { return "PAN_ID outside 0x0001..0xFFFE"; }
    if (r[R_LOCAL] > 0xFFFDU)                                 { return "LOCAL_ADDR outside 0..0xFFFD"; }
    if (r[R_POWER] > 4U)                                      { return "POWER_LEVEL outside 0..4"; }
    if (r[R_BAUD] > 4U)                                       { return "BAUD_RATE_IDX outside 0..4"; }
    if (r[R_PARITY] > 2U)                                     { return "PARITY outside 0..2"; }
    if (r[R_DATA_BITS] != 8U)                                 { return "DATA_BITS must be 8"; }
    if (r[R_STOP_BITS] < 1U || r[R_STOP_BITS] > 2U)           { return "STOP_BITS outside 1..2"; }
    if (r[R_RESP_TO] < 50U || r[R_RESP_TO] > 5000U)           { return "RESPONSE_TIMEOUT_MS outside 50..5000"; }
    if (r[R_IFD] < 10U || r[R_IFD] > 500U)                    { return "INTER_FRAME_DELAY_MS outside 10..500"; }
    if (r[R_RETRY] > 5U)                                      { return "RETRY_COUNT outside 0..5"; }
    if (slave && (r[R_SLAVE_ID] < 1U || r[R_SLAVE_ID] > 247U)) { return "SLAVE_ID outside 1..247"; }
    /* NETWORK_NAME: printable ASCII up to the first NUL, and the 16th byte is the NUL */
    if ((r[R_NAME + NAME_REGS - 1U] & 0xFFU) != 0U)           { return "NETWORK_NAME not NUL-terminated within 16 bytes"; }
    bool ended = false;
    for (unsigned i = 0U; i < PLC_ZIGBEE_NAME_BYTES && !ended; i++) {
        const uint8_t ch = (i % 2U == 0U) ? (uint8_t)(r[R_NAME + i / 2U] >> 8)
                                          : (uint8_t)(r[R_NAME + i / 2U] & 0xFFU);
        if (ch == 0U) { ended = true; }
        else if (ch < 0x20U || ch > 0x7EU) { return "NETWORK_NAME is not printable ASCII"; }
    }
    return NULL;
}

bool splc_zigbee_get_config(splc_zigbee_config_t *out, uint32_t *generation)
{
    if (generation != NULL) { *generation = s_gen; }
    if (out == NULL) { return false; }
    decode_config(s_run, out);
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
    if (!s_registered) { return 0UL; }
    uint32_t caps = PLC_ZIGBEE_CAP_ZIGBEE;
    /* MODBUS_SLAVE (0x4000): only a Slave role the port supports AND the build serves (the
     * endpoint in plc_mbs). MODBUS_MASTER (0x0100) stays with plc_mbm: Master over the Zigbee
     * pipe is not written yet, so it is not declared on Zigbee's behalf. */
    if ((s_ops.roles & SPLC_ZIGBEE_ROLE_SLAVE) != 0U) { caps |= 0x00004000UL; }
    return caps;
}

void plc_zigbee_read(uint16_t offset, uint16_t quantity, uint16_t *registers_out)
{
    splc_zigbee_status_t st;
    memset(&st, 0, sizeof(st));
    if ((unsigned)offset + quantity > R_LINK && (unsigned)offset <= R_ERROR &&
        s_registered && s_ops.status != NULL) {
        if (!s_ops.status(s_ops.ctx, &st)) { memset(&st, 0, sizeof(st)); }
    }
    for (uint16_t k = 0U; k < quantity; k++) {
        const unsigned i = (unsigned)offset + k;
        uint16_t v = 0U;
        if (i < CFG)                { v = is_key_reg(i) ? 0U : s_run[i]; }   /* the key is write-only */
        else if (i == R_STATUS)     { v = key_nonzero(s_run) ? PLC_ZIGBEE_STATUS_KEY_SET : 0U; }
        else if (i == R_LINK)       { v = st.link_state; }
        else if (i == R_RSSI)       { v = (uint16_t)st.rssi_dbm; }
        else if (i == R_LQI)        { v = st.lqi; }
        else if (i == R_TX_PKT)     { v = st.tx_packets; }
        else if (i == R_RX_PKT)     { v = st.rx_packets; }
        else if (i == R_TX_ERR)     { v = st.tx_errors; }
        else if (i == R_RX_ERR)     { v = st.rx_errors; }
        else if (i == R_ERROR)      { v = st.error_code; }
        /* +32..+35, +45..+63: 0 */
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

/* The 32 registers a COMMIT of the draft would store: the stored key is kept unless a new one is
 * taken, and PRESERVE_KEY (a command) is dropped. Used by validate (port hook) and commit. */
static void draft_result(uint16_t *n)
{
    memcpy(n, s_draft, CFG * sizeof(uint16_t));
    const uint16_t c = s_draft[R_CTRL];
    const bool take_new_key = ((c & PLC_ZIGBEE_CTRL_SECURITY) != 0U) &&
                              ((c & PLC_ZIGBEE_CTRL_PRESERVE_KEY) == 0U);
    if (!take_new_key) {                           /* SECURITY = 0, or PRESERVE_KEY: keep the stored key */
        for (unsigned i = 0U; i < KEY_REGS; i++) { n[R_KEY + i] = s_run[R_KEY + i]; }
    }
    n[R_CTRL] = (uint16_t)(c & CTRL_STORE_MASK);   /* PRESERVE_KEY is a command, never stored */
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
        const char *why = enabled_fields_error(s_draft);
        if (why != NULL) {
            log_warn(TAG, "COMMIT rejected: %s", why);
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
    /* Last: what only the port knows (e.g. Slave IDs the module reserves). */
    if (s_registered && s_ops.validate != NULL) {
        uint16_t n[CFG];
        splc_zigbee_config_t cfg;
        draft_result(n);
        decode_config(n, &cfg);
        if (!s_ops.validate(s_ops.ctx, &cfg)) {
            log_warn(TAG, "COMMIT rejected: the port refused the configuration");
            return false;
        }
    }
    return true;
}

void plc_zigbee_commit_draft(void)
{
    if (!s_draft_open) { return; }
    uint16_t n[CFG];
    draft_result(n);

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
    if ((c & PLC_ZIGBEE_CTRL_ENABLE) != 0U) {
        const char *why = enabled_fields_error(n);
        if (why != NULL) {
            log_warn(TAG, "import rejected: %s", why);
            return false;
        }
    }
    if (memcmp(n, s_run, sizeof(n)) != 0) {
        memcpy(s_run, n, sizeof(s_run));
        s_gen++;
    }
    plc_zigbee_discard_draft();
    return true;
}

#endif /* SPLC_ZIGBEE_ENABLE */