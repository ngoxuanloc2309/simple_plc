#include "plc_modbus_cfg.h"

#include <string.h>

#include "nanomodbus.h"
#include "plc_tag.h"
#include "plc_tag_def.h"
#include "plc_rule.h"
#include "sx_time.h"
#include "logger.h"

static const char *TAG = "PLC_MODBUS_CFG";

/*
 * See plc_modbus_cfg.h for the overall design (static address-range
 * table, non-blocking poll every scan cycle, separate RAM staging buffer
 * for the Rule Transfer protocol).
 */

/* --- Owned instances (declared extern in plc_modbus_cfg.h) ------------- */

SPLC_DeviceDescriptor   g_device_descriptor;
SPLC_DeviceResourceInfo g_device_resource_info;
SPLC_DeviceHealth       g_device_health;

/* --- nanoMODBUS server instance and its transport ----------------------
 *
 * s_transport is a copy of the modbus_transport_t the caller passed to
 * plc_modbus_cfg_init(), not a pointer into caller-owned memory -- see
 * plc_modbus_cfg.h's doc comment on plc_modbus_cfg_init(). The concrete
 * driver instance behind s_transport.ctx (a sx_usb_tiny_t*, a sx_uart_t*,
 * or similar, depending on which factory function built the transport)
 * is still not owned here; only the small modbus_transport_t struct
 * itself is copied.
 */

static nmbs_t              s_nmbs;
static modbus_transport_t  s_transport;
static uint32_t            s_boot_tick_ms;

/*
 * true only after nmbs_server_create() has actually succeeded. While
 * false, s_nmbs is still all-zero (nmbs_create() never ran), so every
 * platform.read/write/flush pointer inside it is NULL --
 * modbus_config_service() must NOT call nmbs_server_poll() then, or it
 * jumps to address 0 and HardFaults on the very first scan cycle.
 */
static bool                s_initialized = false;

/*
 * --- Rule Transfer staging state (register map section 9, 0x9000-0xA001) -
 *
 * Per v1.9's own design intent (see docs/architecture.md section 2.6.2,
 * and splc_flash_define.h's comment: "staging happens over Modbus
 * registers in RAM/App side; only the final committed table is written
 * [to Flash]") -- this is a separate RAM buffer, NOT g_rule_table[]
 * (core/plc_rule/plc_rule.c, Layer 2). The App can write partial/
 * in-progress rule data here across many Modbus transactions without
 * ever exposing a half-written table to the live Rule Engine, which
 * keeps evaluating the old g_rule_table[] against real I/O the entire
 * time. rule_table_commit() (Layer 2) is the only thing that ever moves
 * data from here into g_rule_table[], and only after this file has
 * already verified expected_crc16 against a CRC-16/MODBUS computed over
 * exactly rule_count_staged records here.
 *
 * Sized to MAX_RULES (100) * sizeof(SPLC_RuleRecord) (32) = 3200 bytes,
 * matching STAGING_RULE_TABLE's "1600 max" register / 100-rule cap in
 * the register map table.
 */
static SPLC_RuleRecord  s_staging_rule_table[MAX_RULES];
static uint16_t         s_rule_count_staged;
static uint16_t         s_expected_crc16;

/* CONFIG_STATUS values (0x9000, RO) -- section 9's own literal values
 * (0=IDLE, 1=RECEIVING, 2=VERIFYING, 3=READY, 4=ERROR), not a named enum
 * in the spec itself, so kept as plain #defines matching those exact
 * numbers rather than inventing an SPLC_ name the spec doesn't define. */
#define CONFIG_STATUS_IDLE       0U
#define CONFIG_STATUS_RECEIVING  1U
#define CONFIG_STATUS_VERIFYING  2U
#define CONFIG_STATUS_READY      3U
#define CONFIG_STATUS_ERROR      4U

static uint16_t s_config_status = CONFIG_STATUS_IDLE;
static uint16_t s_config_error_code = SPLC_ERROR_NONE;

/* COMMIT_COMMAND magic value, per section 9's table ("Ghi 0xA5A5 de
 * verify CRC va commit"). */
#define COMMIT_COMMAND_MAGIC 0xA5A5U

static uint16_t s_active_rule_version = 0;

/* --- SYSTEM_COMMAND / SYSTEM_COMMAND_RESULT state ----------------------- */

static SPLC_SystemCommandResult s_system_command_result = {
    .status     = SPLC_CMD_STATUS_IDLE,
    .error_code = SPLC_ERROR_NONE,
};

/* --- DEVICE_DESCRIPTOR (0x0000-0x0009, RO, 10 registers) ---------------- */

static void read_device_descriptor(uint16_t offset, uint16_t quantity, uint16_t *registers_out)
{
    const uint16_t *fields = (const uint16_t *)&g_device_descriptor;
    for (uint16_t i = 0; i < quantity; i++) {
        registers_out[i] = fields[offset + i];
    }
}

/* --- DEVICE_RESOURCE_INFO (0x0020-0x0029, RO, 10 registers) ------------- */

static void read_device_resource_info(uint16_t offset, uint16_t quantity, uint16_t *registers_out)
{
    const uint16_t *fields = (const uint16_t *)&g_device_resource_info;
    for (uint16_t i = 0; i < quantity; i++) {
        registers_out[i] = fields[offset + i];
    }
}

/* --- RULE_TABLE_INFO (0x0010, RO, 1 register) --------------------------- */

static void read_rule_table_info(uint16_t offset, uint16_t quantity, uint16_t *registers_out)
{
    (void)offset; /* Single-register block; offset is always 0 here. */
    for (uint16_t i = 0; i < quantity; i++) {
        registers_out[i] = g_rule_count.rule_count;
    }
}

/* --- ACTIVE_RULE_TABLE (0x0100-0x073F, RO, 1600 max registers) ---------- *
 *
 * Address: ActiveRule[i] = 0x0100 + i * 16 (section 7's own formula) --
 * 16 registers (32 bytes) per SPLC_RuleRecord, laid out per section 7's
 * offset table (+0..+1 threshold_lo, +2..+3 threshold_hi, ..., +13..+15
 * reserved). SPLC_RuleRecord's actual C layout (plc_rule.h) already
 * matches that field order byte-for-byte -- reinterpreting it as
 * uint16_t[16] and applying put_u32's high-word-first swap for the
 * three int32_t/uint32_t fields is sufficient; no separate manual field
 * copy is needed.
 */

static void read_active_rule_table(uint16_t address, uint16_t quantity, uint16_t *registers_out)
{
    for (uint16_t i = 0; i < quantity; i++) {
        uint16_t reg_addr   = address + i;
        uint16_t rule_index = (reg_addr - 0x0100U) / 16U;
        uint16_t field_off  = (reg_addr - 0x0100U) % 16U;

        if (rule_index >= MAX_RULES) {
            registers_out[i] = 0;
            continue;
        }

        const SPLC_RuleRecord *r = &g_rule_table[rule_index];
        uint16_t word;

        switch (field_off) {
            case 0: word = (uint16_t)((uint32_t)r->threshold_lo >> 16); break;
            case 1: word = (uint16_t)((uint32_t)r->threshold_lo & 0xFFFFU); break;
            case 2: word = (uint16_t)((uint32_t)r->threshold_hi >> 16); break;
            case 3: word = (uint16_t)((uint32_t)r->threshold_hi & 0xFFFFU); break;
            case 4: word = (uint16_t)(r->for_ms >> 16); break;
            case 5: word = (uint16_t)(r->for_ms & 0xFFFFU); break;
            case 6: word = (uint16_t)((uint32_t)r->action_param >> 16); break;
            case 7: word = (uint16_t)((uint32_t)r->action_param & 0xFFFFU); break;
            case 8: word = r->trigger_tag; break;
            case 9: word = r->action_tag; break;
            case 10: word = r->guard_tag; break;
            case 11: word = (uint16_t)((r->enabled << 8) | r->trigger_type); break;
            case 12: word = (uint16_t)((r->compare_op << 8) | r->action_type); break;
            default: word = 0; break; /* +13..+15: reserved, sender writes 0 */
        }

        registers_out[i] = word;
    }
}

/* --- DEVICE_HEALTH (0x0800-0x0809, RO, 10 registers) --------------------
 *
 * g_device_health.uptime_s is refreshed lazily here, at read time, rather
 * than incremented every scan cycle in modbus_config_service() -- the App
 * only ever observes it through a read, so there is no benefit to paying
 * a subtraction+division every 10 ms scan cycle just to keep a value
 * current that nothing else in the firmware ever consults.
 */

static void read_device_health(uint16_t offset, uint16_t quantity, uint16_t *registers_out)
{
    g_device_health.uptime_s = (sx_get_tick_ms() - s_boot_tick_ms) / 1000U;

    const uint16_t *fields = (const uint16_t *)&g_device_health;
    for (uint16_t i = 0; i < quantity; i++) {
        registers_out[i] = fields[offset + i];
    }
}

/* --- RUNTIME_TAG_VALUES (0x0900-0x09FF, RO, 256 max registers, 2 reg/tag) */

static void read_runtime_tag_values(uint16_t address, uint16_t quantity, uint16_t *registers_out)
{
    for (uint16_t i = 0; i < quantity; i++) {
        uint16_t reg_addr = address + i;
        uint16_t tag_idx  = (reg_addr - 0x0900U) / 2U;
        bool     is_high  = ((reg_addr - 0x0900U) % 2U) == 0U;

        if (tag_idx >= MAX_TAGS) {
            registers_out[i] = 0;
            continue;
        }

        int32_t value = tag_read(tag_idx);
        registers_out[i] = is_high ? (uint16_t)((uint32_t)value >> 16)
                                    : (uint16_t)((uint32_t)value & 0xFFFFU);
    }
}

/* --- SYSTEM_COMMAND (0x0A00, WO) / SYSTEM_COMMAND_RESULT (0x0A01-0x0A02, RO) */

static void read_system_command_result(uint16_t offset, uint16_t quantity, uint16_t *registers_out)
{
    const uint16_t *fields = (const uint16_t *)&s_system_command_result;
    for (uint16_t i = 0; i < quantity; i++) {
        registers_out[i] = fields[offset + i];
    }
}

/*
 * Only status is flipped to ACCEPTED/BUSY/DONE here -- actually
 * performing SPLC_SYSTEM_CMD_REBOOT/FACTORY_RESET/CLEAR_RULES/
 * CLEAR_RETAIN (NVIC_SystemReset(), sx_flash_erase(), ...) calls into
 * Layer 0/1 functionality this file does not own or call directly. Per
 * plc_system_cmd.h's own comment, that execution belongs in Layer 4 --
 * this function only decodes the command and hands it off.
 *
 * TODO: no Layer 4 handler exists yet to actually consume
 * s_system_command_result once status == SPLC_CMD_STATUS_ACCEPTED and
 * perform the command. Until that lands, every command is accepted here
 * (status flips to ACCEPTED) but nothing further happens -- the MCU does
 * not actually reboot, wipe Flash, or clear tables yet.
 */
static void write_system_command(uint16_t value)
{
    SPLC_SystemCommand cmd = (SPLC_SystemCommand)value;

    switch (cmd) {
        case SPLC_SYSTEM_CMD_REBOOT:
        case SPLC_SYSTEM_CMD_FACTORY_RESET:
        case SPLC_SYSTEM_CMD_CLEAR_RULES:
        case SPLC_SYSTEM_CMD_CLEAR_RETAIN:
            s_system_command_result.status     = SPLC_CMD_STATUS_ACCEPTED;
            s_system_command_result.error_code = SPLC_ERROR_NONE;
            break;
        default:
            s_system_command_result.status     = SPLC_CMD_STATUS_ERROR;
            s_system_command_result.error_code = SPLC_ERROR_INVALID_COMMAND;
            break;
    }
}

/* --- Rule Transfer: CONFIG_STATUS/CONFIG_ERROR_CODE (0x9000-0x9001, RO) - */

static void read_config_status(uint16_t offset, uint16_t quantity, uint16_t *registers_out)
{
    (void)offset;
    for (uint16_t i = 0; i < quantity; i++) {
        registers_out[i] = s_config_status;
    }
}

static void read_config_error_code(uint16_t offset, uint16_t quantity, uint16_t *registers_out)
{
    (void)offset;
    for (uint16_t i = 0; i < quantity; i++) {
        registers_out[i] = s_config_error_code;
    }
}

/* --- Rule Transfer: RULE_COUNT_STAGED / EXPECTED_CRC16 (0x9002-0x9003, RW) */

static void read_rule_count_staged(uint16_t offset, uint16_t quantity, uint16_t *registers_out)
{
    (void)offset;
    for (uint16_t i = 0; i < quantity; i++) {
        registers_out[i] = s_rule_count_staged;
    }
}

static void write_rule_count_staged_value(uint16_t value)
{
    /* A count exceeding MAX_RULES can never be committed successfully
     * (rule_table_commit() would refuse it) -- reject here rather than
     * accepting an unreachable value and only discovering the problem
     * at COMMIT_COMMAND time. */
    if (value > MAX_RULES) {
        log_warn(TAG, "rule_count_staged=%u rejected (> MAX_RULES=%u)",
                 value, (unsigned)MAX_RULES);
        s_config_status     = CONFIG_STATUS_ERROR;
        s_config_error_code = SPLC_ERROR_INVALID_PARAMETER;
        return;
    }

    log_debug(TAG, "rule_count_staged=%u, status -> RECEIVING", value);
    s_rule_count_staged = value;
    s_config_status      = CONFIG_STATUS_RECEIVING;
    s_config_error_code  = SPLC_ERROR_NONE;
}

static void read_expected_crc16(uint16_t offset, uint16_t quantity, uint16_t *registers_out)
{
    (void)offset;
    for (uint16_t i = 0; i < quantity; i++) {
        registers_out[i] = s_expected_crc16;
    }
}

static void write_expected_crc16_value(uint16_t value)
{
    s_expected_crc16 = value;
}

/* --- Rule Transfer: ACTIVE_RULE_COUNT / ACTIVE_RULE_CRC16 (0x9004-0x9005, RO) */

static void read_active_rule_count(uint16_t offset, uint16_t quantity, uint16_t *registers_out)
{
    (void)offset;
    for (uint16_t i = 0; i < quantity; i++) {
        registers_out[i] = g_rule_count.rule_count;
    }
}

/* --- Wire-format CRC (section 8.4) -------------------------------------
 *
 * EXPECTED_CRC16 / ACTIVE_RULE_CRC16 are defined over the SERIALIZED
 * rule table: rule_count x 32 bytes laid out exactly as the App sends
 * them (each 16-bit register high byte first, each 32-bit field High
 * Word then Low Word -- section 8.4). That is NOT the in-RAM byte image
 * of SPLC_RuleRecord: on a little-endian Cortex-M33 the RAM bytes of
 * every multi-byte field are reversed relative to the wire, so hashing
 * the struct directly gives a different CRC than the App computes over
 * what it sent (verified: the same rule hashed 0x6575 from RAM bytes vs
 * 0x18A4 from wire bytes). The CRC therefore has to be taken over the
 * wire image, produced by rule_record_to_wire() below -- the exact
 * inverse of write_staging_rule_table()'s field mapping.
 *
 * nmbs_crc_calc() is deliberately NOT used here: it returns the CRC with
 * its two bytes swapped (it is built for RTU framing, low byte first),
 * so it is not the plain CRC-16/MODBUS value the register holds.
 */

static uint16_t crc16_modbus_update(uint16_t crc, uint8_t byte)
{
    crc ^= byte;
    for (uint8_t bit = 0; bit < 8U; bit++) {
        crc = (crc & 1U) ? (uint16_t)((crc >> 1) ^ 0xA001U) : (uint16_t)(crc >> 1);
    }
    return crc;
}

/* Serialize one record to its 32-byte wire image (16 registers). */
static void rule_record_to_wire(const SPLC_RuleRecord *r, uint8_t out[32])
{
    uint16_t w[16];
    w[0]  = (uint16_t)((uint32_t)r->threshold_lo >> 16);
    w[1]  = (uint16_t)((uint32_t)r->threshold_lo & 0xFFFFU);
    w[2]  = (uint16_t)((uint32_t)r->threshold_hi >> 16);
    w[3]  = (uint16_t)((uint32_t)r->threshold_hi & 0xFFFFU);
    w[4]  = (uint16_t)(r->for_ms >> 16);
    w[5]  = (uint16_t)(r->for_ms & 0xFFFFU);
    w[6]  = (uint16_t)((uint32_t)r->action_param >> 16);
    w[7]  = (uint16_t)((uint32_t)r->action_param & 0xFFFFU);
    w[8]  = r->trigger_tag;
    w[9]  = r->action_tag;
    w[10] = r->guard_tag;
    w[11] = (uint16_t)(((uint16_t)r->enabled << 8) | r->trigger_type);
    w[12] = (uint16_t)(((uint16_t)r->compare_op << 8) | r->action_type);
    w[13] = 0; w[14] = 0; w[15] = 0;   /* reserved[6]: sender writes 0 */

    for (uint8_t i = 0; i < 16U; i++) {
        out[2U * i]      = (uint8_t)(w[i] >> 8);
        out[2U * i + 1U] = (uint8_t)(w[i] & 0xFFU);
    }
}

/* CRC-16/MODBUS over `count` records' wire images (count x 32 bytes). */
static uint16_t rule_table_wire_crc16(const SPLC_RuleRecord *table, uint16_t count)
{
    uint16_t crc = 0xFFFFU;
    uint8_t  buf[32];

    for (uint16_t i = 0; i < count; i++) {
        rule_record_to_wire(&table[i], buf);
        for (uint8_t b = 0; b < 32U; b++) {
            crc = crc16_modbus_update(crc, buf[b]);
        }
    }
    return crc;
}

static void read_active_rule_crc16(uint16_t offset, uint16_t quantity, uint16_t *registers_out)
{
    (void)offset;
    uint16_t crc = rule_table_wire_crc16((const SPLC_RuleRecord *)g_rule_table,
                                          (uint16_t)g_rule_count.rule_count);
    for (uint16_t i = 0; i < quantity; i++) {
        registers_out[i] = crc;
    }
}

/* --- Rule Transfer: STAGING_RULE_TABLE (0x9010-0x964F, RW, 1600 max) ----- *
 *
 * Same per-record layout and word order as ACTIVE_RULE_TABLE above, just
 * addressed from 0x9010 (StagingRule[i] = 0x9010 + i * 16, section 7)
 * and read/written against s_staging_rule_table[] instead of
 * g_rule_table[].
 */

static void read_staging_rule_table(uint16_t address, uint16_t quantity, uint16_t *registers_out)
{
    for (uint16_t i = 0; i < quantity; i++) {
        uint16_t reg_addr   = address + i;
        uint16_t rule_index = (reg_addr - 0x9010U) / 16U;
        uint16_t field_off  = (reg_addr - 0x9010U) % 16U;

        if (rule_index >= MAX_RULES) {
            registers_out[i] = 0;
            continue;
        }

        const SPLC_RuleRecord *r = &s_staging_rule_table[rule_index];
        uint16_t word;

        switch (field_off) {
            case 0: word = (uint16_t)((uint32_t)r->threshold_lo >> 16); break;
            case 1: word = (uint16_t)((uint32_t)r->threshold_lo & 0xFFFFU); break;
            case 2: word = (uint16_t)((uint32_t)r->threshold_hi >> 16); break;
            case 3: word = (uint16_t)((uint32_t)r->threshold_hi & 0xFFFFU); break;
            case 4: word = (uint16_t)(r->for_ms >> 16); break;
            case 5: word = (uint16_t)(r->for_ms & 0xFFFFU); break;
            case 6: word = (uint16_t)((uint32_t)r->action_param >> 16); break;
            case 7: word = (uint16_t)((uint32_t)r->action_param & 0xFFFFU); break;
            case 8: word = r->trigger_tag; break;
            case 9: word = r->action_tag; break;
            case 10: word = r->guard_tag; break;
            case 11: word = (uint16_t)((r->enabled << 8) | r->trigger_type); break;
            case 12: word = (uint16_t)((r->compare_op << 8) | r->action_type); break;
            default: word = 0; break;
        }

        registers_out[i] = word;
    }
}

static void write_staging_rule_table(uint16_t address, uint16_t quantity, const uint16_t *registers)
{
    for (uint16_t i = 0; i < quantity; i++) {
        uint16_t reg_addr   = address + i;
        uint16_t rule_index = (reg_addr - 0x9010U) / 16U;
        uint16_t field_off  = (reg_addr - 0x9010U) % 16U;

        if (rule_index >= MAX_RULES) {
            continue; /* Out-of-range write silently ignored, same as ACTIVE_RULE_TABLE's read side. */
        }

        SPLC_RuleRecord *r = &s_staging_rule_table[rule_index];
        uint16_t word = registers[i];

        /* 32-bit fields need both halves before they can be assembled;
         * store the high half now and only combine on the low half
         * (field_off odd), matching the "High Word -> Low Word" transfer
         * order the App is expected to write in. If the App ever writes
         * out of order within a single record, the affected 32-bit
         * field is simply stale until both halves arrive -- acceptable
         * because rule_table_commit() only runs after the App has
         * already written every register up to rule_count_staged and
         * the CRC-16 over the whole staged table has been verified to
         * match expected_crc16 (see write_commit_command() below); a
         * CRC mismatch from out-of-order writes is caught there, not
         * silently accepted.
         */
        switch (field_off) {
            case 0: r->threshold_lo = (int32_t)((uint32_t)word << 16) | (r->threshold_lo & 0xFFFF); break;
            case 1: r->threshold_lo = (int32_t)(((uint32_t)r->threshold_lo & 0xFFFF0000U) | word); break;
            case 2: r->threshold_hi = (int32_t)((uint32_t)word << 16) | (r->threshold_hi & 0xFFFF); break;
            case 3: r->threshold_hi = (int32_t)(((uint32_t)r->threshold_hi & 0xFFFF0000U) | word); break;
            case 4: r->for_ms = ((uint32_t)word << 16) | (r->for_ms & 0xFFFFU); break;
            case 5: r->for_ms = (r->for_ms & 0xFFFF0000U) | word; break;
            case 6: r->action_param = (int32_t)((uint32_t)word << 16) | (r->action_param & 0xFFFF); break;
            case 7: r->action_param = (int32_t)(((uint32_t)r->action_param & 0xFFFF0000U) | word); break;
            case 8: r->trigger_tag = word; break;
            case 9: r->action_tag = word; break;
            case 10: r->guard_tag = word; break;
            case 11: r->enabled = (uint8_t)(word >> 8); r->trigger_type = (uint8_t)(word & 0xFFU); break;
            case 12: r->compare_op = (uint8_t)(word >> 8); r->action_type = (uint8_t)(word & 0xFFU); break;
            default: break; /* +13..+15: reserved, receiver ignores per section 6's encoding table. */
        }
    }
}

/* --- Rule Transfer: COMMIT_COMMAND / ACTIVE_RULE_VERSION (0xA000-0xA001) - */

static void read_active_rule_version(uint16_t offset, uint16_t quantity, uint16_t *registers_out)
{
    (void)offset;
    for (uint16_t i = 0; i < quantity; i++) {
        registers_out[i] = s_active_rule_version;
    }
}

/*
 * One debug line per active rule, right after a successful commit, so the
 * serial log shows WHAT was loaded (not just that something was). Capped
 * so a full 100-rule table cannot flood the log / stall the scan cycle
 * (each log line goes out over the same USB the App is polling).
 */
#define RULE_SUMMARY_MAX_LINES 10U

static void log_rule_summary(void)
{
    uint16_t count = g_rule_count.rule_count;
    uint16_t shown = (count < RULE_SUMMARY_MAX_LINES) ? count : (uint16_t)RULE_SUMMARY_MAX_LINES;

    for (uint16_t i = 0; i < shown; i++) {
        const SPLC_RuleRecord *r = &g_rule_table[i];
        log_debug(TAG, "  rule[%u] en=%u trig_tag=%u trig_type=%u cmp=%u for_ms=%lu "
                       "guard=0x%04X action=%u action_tag=%u param=%ld",
                  i, r->enabled, r->trigger_tag, r->trigger_type, r->compare_op,
                  (unsigned long)r->for_ms, r->guard_tag, r->action_type,
                  r->action_tag, (long)r->action_param);
    }
    if (count > shown) {
        log_debug(TAG, "  ... %u more rule(s) not listed", (unsigned)(count - shown));
    }
}

/*
 * Section 6's commit protocol, step 4 ("Verify + swap"): CRC-16/MODBUS
 * over exactly s_rule_count_staged * sizeof(SPLC_RuleRecord) staged
 * bytes must match s_expected_crc16 before rule_table_commit() (Layer 2)
 * is ever called. On mismatch, the Active Rule Table is left completely
 * untouched (g_rule_table[] via rule_table_commit() is simply never
 * invoked) and CONFIG_ERROR_CODE reports SPLC_ERROR_CRC_MISMATCH -- the
 * App is expected to notice CONFIG_STATUS == ERROR and re-stage.
 */
static void write_commit_command(uint16_t value)
{
    if (value != COMMIT_COMMAND_MAGIC) {
        log_warn(TAG, "commit rejected: bad magic 0x%04X (expected 0x%04X)",
                 value, COMMIT_COMMAND_MAGIC);
        s_config_status     = CONFIG_STATUS_ERROR;
        s_config_error_code = SPLC_ERROR_INVALID_PARAMETER;
        return;
    }

    s_config_status = CONFIG_STATUS_VERIFYING;
    log_debug(TAG, "commit: verifying CRC, rule_count_staged=%u expected_crc16=0x%04X",
              s_rule_count_staged, s_expected_crc16);

    uint16_t actual_crc16 = rule_table_wire_crc16(s_staging_rule_table,
                                                   s_rule_count_staged);

    if (actual_crc16 != s_expected_crc16) {
        log_warn(TAG, "commit rejected: CRC mismatch, actual=0x%04X expected=0x%04X",
                 actual_crc16, s_expected_crc16);
        s_config_status     = CONFIG_STATUS_ERROR;
        s_config_error_code = SPLC_ERROR_CRC_MISMATCH;
        return;
    }

    bool ok = rule_table_commit((const uint8_t *)s_staging_rule_table, s_rule_count_staged);

    if (!ok) {
        log_warn(TAG, "commit rejected: rule_table_commit() returned false "
                 "(rule_count_staged=%u)", s_rule_count_staged);
        s_config_status     = CONFIG_STATUS_ERROR;
        s_config_error_code = SPLC_ERROR_INVALID_PARAMETER; /* e.g. rule_count_staged > MAX_RULES */
        return;
    }

    s_config_status     = CONFIG_STATUS_READY;
    s_config_error_code = SPLC_ERROR_NONE;
    s_active_rule_version++;
    log_info(TAG, "RULE UPLOAD DONE: %u rule(s) loaded, crc16=0x%04X, active_rule_version=%u",
             s_rule_count_staged, actual_crc16, s_active_rule_version);
    log_rule_summary();
}

/*
 * write_multi_fn adapters for the two exactly-1-register RW blocks.
 * Both blocks are 1 register wide, so `quantity` is always 1 here
 * (cb_write_multiple_registers() clamps each pass to the block's own
 * end_addr) and only registers[0] is meaningful.
 */
static void write_rule_count_staged(uint16_t address, uint16_t quantity, const uint16_t *registers)
{
    (void)address;
    if (quantity >= 1U) {
        write_rule_count_staged_value(registers[0]);
    }
}

static void write_expected_crc16(uint16_t address, uint16_t quantity, const uint16_t *registers)
{
    (void)address;
    if (quantity >= 1U) {
        write_expected_crc16_value(registers[0]);
    }
}

/* --- Address-range dispatch table ---------------------------------------
 *
 * One entry per register-map block (section 8/9's tables). read_holding_
 * registers()/write_multiple_registers() below just walk this table
 * looking for the entry whose [start_addr, end_addr] covers the
 * request's address, then delegate -- new blocks (e.g. a future Gateway
 * SKU's MB_COIL/MB_HOLDING passthrough) are added as one more entry here
 * rather than growing a per-callback switch/case. write_cb is NULL for
 * every RO block; write attempts against those are rejected below with
 * NMBS_EXCEPTION_ILLEGAL_DATA_ADDRESS before ever reaching this table's
 * write_multi_cb (write_single_register requests are declined outright --
 * see write_single_register() below, every RW block here is only ever
 * written via write_multiple_registers per the App's own FC16 usage in
 * section 6's protocol steps).
 */

typedef void (*read_block_fn)(uint16_t offset_or_addr, uint16_t quantity, uint16_t *registers_out);
typedef void (*write_multi_fn)(uint16_t address, uint16_t quantity, const uint16_t *registers);

typedef struct {
    uint16_t        start_addr;
    uint16_t        end_addr;   /* Inclusive */
    bool            addr_is_offset; /* true: callback gets (addr - start_addr); false: callback gets raw addr (ACTIVE_RULE_TABLE/STAGING_RULE_TABLE/RUNTIME_TAG_VALUES need the raw address to compute rule_index/tag_idx) */
    read_block_fn   read_cb;
    write_multi_fn  write_multi_cb; /* Used by BOTH FC16 and FC06 (see cb_write_single_register) */
} modbus_block_t;

static const modbus_block_t s_blocks[] = {
    { 0x0000, 0x0009, true,  read_device_descriptor,      NULL },
    { 0x0010, 0x0010, true,  read_rule_table_info,        NULL },
    { 0x0020, 0x0029, true,  read_device_resource_info,   NULL },
    { 0x0100, 0x073F, false, read_active_rule_table,      NULL },
    { 0x0800, 0x0809, true,  read_device_health,          NULL },
    { 0x0900, 0x09FF, false, read_runtime_tag_values,     NULL },
    { 0x0A01, 0x0A02, true,  read_system_command_result,  NULL },

    { 0x9000, 0x9000, true,  read_config_status,          NULL },
    { 0x9001, 0x9001, true,  read_config_error_code,      NULL },
    { 0x9002, 0x9002, true,  read_rule_count_staged,      write_rule_count_staged },
    { 0x9003, 0x9003, true,  read_expected_crc16,         write_expected_crc16 },
    { 0x9004, 0x9004, true,  read_active_rule_count,      NULL },
    { 0x9005, 0x9005, true,  read_active_rule_crc16,      NULL },
    { 0x9010, 0x964F, false, read_staging_rule_table,     write_staging_rule_table },
    { 0xA001, 0xA001, true,  read_active_rule_version,    NULL },
};

#define NUM_BLOCKS (sizeof(s_blocks) / sizeof(s_blocks[0]))

static const modbus_block_t *find_block(uint16_t address)
{
    for (size_t i = 0; i < NUM_BLOCKS; i++) {
        if (address >= s_blocks[i].start_addr && address <= s_blocks[i].end_addr) {
            return &s_blocks[i];
        }
    }
    return NULL;
}

/*
 * A request may span into an address range this table has no entry for
 * (e.g. reading across a RESERVED gap like 0x0011-0x001F, or a quantity
 * that runs past a block's end_addr). Rather than rejecting the whole
 * request, every unmapped register is filled with 0 -- matching section
 * 9's own note for the config-transfer RESERVED block ("ghi/doc bo qua")
 * and giving the same "reads as zero" behavior for every other gap too.
 */

/* --- nanoMODBUS callbacks ------------------------------------------------ */

static nmbs_error cb_read_holding_registers(uint16_t address, uint16_t quantity,
                                             uint16_t *registers_out, uint8_t unit_id, void *arg)
{
    (void)unit_id;
    (void)arg;

    uint16_t filled = 0;
    while (filled < quantity) {
        uint16_t cur_addr = address + filled;
        const modbus_block_t *block = find_block(cur_addr);

        if (block == NULL || block->read_cb == NULL) {
            registers_out[filled] = 0; /* Unmapped/gap register reads as 0 -- see note above. */
            filled++;
            continue;
        }

        /* Clamp this pass to not run past the block's own end_addr, so
         * a request spanning two adjacent blocks (or a block followed
         * by a gap) is served correctly one block at a time. */
        uint16_t remaining_in_block = block->end_addr - cur_addr + 1U;
        uint16_t remaining_in_req   = quantity - filled;
        uint16_t chunk = (remaining_in_block < remaining_in_req) ? remaining_in_block : remaining_in_req;

        uint16_t offset_or_addr = block->addr_is_offset ? (cur_addr - block->start_addr) : cur_addr;
        block->read_cb(offset_or_addr, chunk, &registers_out[filled]);

        filled += chunk;
    }

    return NMBS_ERROR_NONE;
}

static nmbs_error cb_write_multiple_registers(uint16_t address, uint16_t quantity,
                                               const uint16_t *registers, uint8_t unit_id, void *arg)
{
    (void)unit_id;
    (void)arg;

    uint16_t written = 0;
    while (written < quantity) {
        uint16_t cur_addr = address + written;
        const modbus_block_t *block = find_block(cur_addr);

        if (block == NULL || block->write_multi_cb == NULL) {
            /* Write to a RO block or an unmapped gap -- per Modbus
             * convention this should really be a distinct per-register
             * exception, but nanoMODBUS's callback contract here is
             * whole-request-or-nothing (no partial-exception return).
             * Rejecting the entire request with ILLEGAL_DATA_ADDRESS is
             * the safest choice: it never silently accepts a write the
             * App believed succeeded. */
            log_warn(TAG, "write rejected: address=0x%04X quantity=%u is RO/unmapped",
                     cur_addr, quantity);
            return NMBS_EXCEPTION_ILLEGAL_DATA_ADDRESS;
        }

        uint16_t remaining_in_block = block->end_addr - cur_addr + 1U;
        uint16_t remaining_in_req   = quantity - written;
        uint16_t chunk = (remaining_in_block < remaining_in_req) ? remaining_in_block : remaining_in_req;

        block->write_multi_cb(cur_addr, chunk, &registers[written]);

        written += chunk;
    }

    return NMBS_ERROR_NONE;
}

/*
 * FC06 (Write Single Register). SYSTEM_COMMAND (0x0A00) and
 * COMMIT_COMMAND (0xA000) are both WO, single-register, and documented
 * in section 6/8 as written via "FC16/FC06", so they are handled
 * directly here (they have no read_cb and are not in s_blocks).
 *
 * Every other writable register (RULE_COUNT_STAGED, EXPECTED_CRC16,
 * STAGING_RULE_TABLE) is an RW block in s_blocks; FC06 to those is
 * forwarded to the same table-driven path as FC16 with quantity=1.
 * Anything not covered by either is rejected with
 * ILLEGAL_DATA_ADDRESS.
 */
static nmbs_error cb_write_single_register(uint16_t address, uint16_t value, uint8_t unit_id, void *arg)
{
    (void)unit_id;
    (void)arg;

    switch (address) {
        case 0x0A00: write_system_command((uint16_t)value); return NMBS_ERROR_NONE;
        case 0xA000: write_commit_command((uint16_t)value); return NMBS_ERROR_NONE;
        default:     break;
    }

    /* Every other writable register (RULE_COUNT_STAGED, EXPECTED_CRC16,
     * STAGING_RULE_TABLE) lives in s_blocks. FC06 is just FC16 with
     * quantity=1, so route it through the same table-driven path rather
     * than rejecting it: a client (e.g. pymodbus write_register()) is
     * entitled to use either function code for a 1-register RW block. */
    return cb_write_multiple_registers(address, 1U, &value, unit_id, arg);
}

/* --- Public API (see plc_modbus_cfg.h for full contracts) --------------- */

void plc_modbus_cfg_init(const modbus_transport_t *transport)
{
    s_initialized  = false;
    s_transport    = *transport;
    s_boot_tick_ms = sx_get_tick_ms();

    nmbs_platform_conf platform_conf;
    nmbs_platform_conf_create(&platform_conf);
    platform_conf.transport = (nmbs_transport)s_transport.kind;
    platform_conf.read      = s_transport.read;
    platform_conf.write     = s_transport.write;
    platform_conf.arg       = s_transport.ctx;

    nmbs_callbacks callbacks;
    nmbs_callbacks_create(&callbacks);
    callbacks.read_holding_registers   = cb_read_holding_registers;
    callbacks.write_multiple_registers = cb_write_multiple_registers;
    callbacks.write_single_register    = cb_write_single_register;

    /*
     * address_rtu = transport->unit_id. nanoMODBUS REJECTS 0 on RTU
     * (0 is the broadcast address) with NMBS_ERROR_INVALID_ARGUMENT,
     * returned before nmbs_create() runs -- and it FILTERS on this value
     * at runtime: a request whose unit_id byte differs is silently
     * ignored. So it is not "accepted but not checked"; the App must
     * send exactly this value. See modbus_transport.h's unit_id.
     *
     * The return value MUST be checked: ignoring it (as an earlier
     * version did) left s_nmbs all-zero after a failed create, and the
     * first nmbs_server_poll() then called a NULL platform.read pointer
     * -> HardFault, while the log still claimed "init OK".
     */
    nmbs_error err = nmbs_server_create(&s_nmbs, s_transport.unit_id,
                                        &platform_conf, &callbacks);
    if (err != NMBS_ERROR_NONE) {
        log_error(TAG, "nmbs_server_create failed: err=%d (%s), unit_id=%u "
                       "transport kind=%d -- Modbus config service DISABLED",
                  (int)err, nmbs_strerror(err), (unsigned)s_transport.unit_id,
                  (int)s_transport.kind);
        return;
    }

    /* Non-blocking poll: both the byte-level read/write timeout and the
     * read-timeout used while waiting for a new request are 0, so
     * nmbs_server_poll() (called every scan cycle from
     * modbus_config_service() below) never blocks the 10 ms scan budget
     * waiting on the App. */
    nmbs_set_read_timeout(&s_nmbs, 0);
    nmbs_set_byte_timeout(&s_nmbs, 0);

    s_initialized = true;
    log_info(TAG, "init OK, transport kind=%d unit_id=%u",
             (int)s_transport.kind, (unsigned)s_transport.unit_id);
}

void modbus_config_service(void)
{
    /* Still pump the transport (e.g. tud_task() for USB-CDC) even if the
     * Modbus server failed to initialize, so the USB stack itself keeps
     * running and enumeration is not affected by a Modbus config error. */
    if (s_transport.process != NULL) {
        s_transport.process(s_transport.ctx);
    }

    if (!s_initialized) {
        return;   /* s_nmbs is all-zero; polling it would jump to NULL. */
    }

    nmbs_server_poll(&s_nmbs);
}

void plc_modbus_cfg_record_scan_time(uint32_t scan_time_ms)
{
    g_device_health.scan_time_ms = scan_time_ms;
    if (scan_time_ms > g_device_health.max_scan_time_ms) {
        g_device_health.max_scan_time_ms = scan_time_ms;
    }
}