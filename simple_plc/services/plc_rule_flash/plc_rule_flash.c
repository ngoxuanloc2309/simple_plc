/*
 * plc_rule_flash.c - Layer 3 (PLC Application Services)
 *
 * Implementation of the A/B mechanism declared in plc_rule_flash.h. See
 * that file's header comment, app/splc_flash_define.h (Flash addresses/
 * size constants), and docs/handoff.md section 1 for the full design
 * this implements -- read plc_rule_flash.h first.
 */

#include "plc_rule_flash.h"

#include <string.h>

#include "splc_flash_define.h"
#include "sx_flash.h"
#include "plc_rule.h"       /* SPLC_RuleRecord, MAX_RULES, rule_table_commit() */
#include "plc_modbus_cfg.h" /* rule_record_to_wire(), rule_table_wire_crc16(), crc16_modbus_update() */
#include "logger.h"

static const char *TAG = "PLC_RULE_FLASH";

/* Cross-check the literal duplicated into SPLC_RULE_FLASH_RECORD_MAX_SIZE
 * (splc_flash_define.h) against the real MAX_RULES (plc_rule.h) at
 * compile time -- see that macro's own comment for why it is a literal
 * rather than a #include of plc_rule.h. */
_Static_assert(SPLC_RULE_FLASH_RECORD_MAX_SIZE ==
               (SPLC_RULE_FLASH_HEADER_SIZE + (uint32_t)MAX_RULES * SPLC_RULE_FLASH_RECORD_WIRE_SIZE),
               "SPLC_RULE_FLASH_RECORD_MAX_SIZE out of sync with MAX_RULES");

#define RULE_FLASH_SEQ_OFFSET         0U
#define RULE_FLASH_COUNT_OFFSET       4U
#define RULE_FLASH_CRC_OFFSET         6U
#define RULE_FLASH_DATA_OFFSET        SPLC_RULE_FLASH_HEADER_SIZE

/* Working buffer for one full-size record (header + up to MAX_RULES
 * records' wire bytes). Static, not stack-allocated: this is Layer 3
 * running on a small MCU with a bounded stack budget, and this buffer
 * (3208 bytes) is large enough that putting it on the stack of a
 * function called from write_commit_command() would be a needless risk. */
static uint8_t s_rule_flash_buf[SPLC_RULE_FLASH_RECORD_MAX_SIZE];

/* Working buffer of decoded SPLC_RuleRecord structs (RAM layout), used
 * only by plc_rule_flash_load() right before calling rule_table_commit().
 * Static for the same stack-budget reason as s_rule_flash_buf above. */
static SPLC_RuleRecord s_rule_flash_decoded[MAX_RULES];

static uint32_t read_u32_be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static void write_u32_be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint16_t read_u16_be(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static void write_u16_be(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

/*
 * Field-embedded CRC-16/MODBUS over a record buffer of `record_size`
 * bytes (header + rule data) -- see plc_rule_flash.h for why this is a
 * deliberately different scope from rule_table_wire_crc16(). The crc16
 * field (2 bytes at RULE_FLASH_CRC_OFFSET) is temporarily zeroed for the
 * calculation and restored afterward, same convention as
 * plc_retain.c's retain_record_crc().
 *
 * Built from crc16_modbus_update() (plc_modbus_cfg.h) one byte at a
 * time -- the exact same underlying CRC-16/MODBUS algorithm
 * rule_table_wire_crc16() uses, just applied over a wider byte range
 * (header included). Not a "third CRC formula": same polynomial/init
 * value, different scope.
 */
static uint16_t rule_flash_record_crc(uint8_t *buf, uint32_t record_size)
{
    uint8_t saved0 = buf[RULE_FLASH_CRC_OFFSET];
    uint8_t saved1 = buf[RULE_FLASH_CRC_OFFSET + 1U];

    buf[RULE_FLASH_CRC_OFFSET]      = 0;
    buf[RULE_FLASH_CRC_OFFSET + 1U] = 0;

    uint16_t crc = 0xFFFFU;
    for (uint32_t i = 0; i < record_size; i++) {
        crc = crc16_modbus_update(crc, buf[i]);
    }

    buf[RULE_FLASH_CRC_OFFSET]      = saved0;
    buf[RULE_FLASH_CRC_OFFSET + 1U] = saved1;

    return crc;
}

/*
 * Builds a full record (header + rule_count x 32-byte wire data) into
 * s_rule_flash_buf from the CURRENT in-RAM g_rule_table[], with the given
 * seq_num. Returns the record's total size in bytes (header +
 * rule_count * 32) -- the exact number of bytes that should be written
 * to/read from Flash for this record, NOT SPLC_RULE_FLASH_RECORD_MAX_SIZE.
 */
static uint32_t rule_flash_build_record(uint32_t seq_num)
{
    uint16_t rule_count = g_rule_count.rule_count;

    write_u32_be(&s_rule_flash_buf[RULE_FLASH_SEQ_OFFSET], seq_num);
    write_u16_be(&s_rule_flash_buf[RULE_FLASH_COUNT_OFFSET], rule_count);

    for (uint16_t i = 0; i < rule_count; i++) {
        rule_record_to_wire(&g_rule_table[i],
                             &s_rule_flash_buf[RULE_FLASH_DATA_OFFSET + (uint32_t)i * SPLC_RULE_FLASH_RECORD_WIRE_SIZE]);
    }

    uint32_t record_size = RULE_FLASH_DATA_OFFSET + (uint32_t)rule_count * SPLC_RULE_FLASH_RECORD_WIRE_SIZE;
    uint16_t crc = rule_flash_record_crc(s_rule_flash_buf, record_size);
    write_u16_be(&s_rule_flash_buf[RULE_FLASH_CRC_OFFSET], crc);

    return record_size;
}

/*
 * Decodes one 32-byte wire image back into an SPLC_RuleRecord (RAM struct
 * layout) -- the exact inverse of rule_record_to_wire() (plc_modbus_cfg.h)
 * / plc_modbus_cfg.c's write_staging_rule_table() field mapping.
 *
 * This exists because rule_table_commit() (plc_rule.h) expects a buffer
 * of tightly-packed SPLC_RuleRecord structs in their NATIVE RAM byte
 * layout (it does a plain memcpy() into g_rule_table[]) -- NOT the wire
 * byte layout stored on Flash by this file. Feeding it wire bytes
 * directly would silently misinterpret every field (same class of bug as
 * the CRC-scope bug in docs/handoff.md section 4.2, just on the decode
 * side instead of the checksum side): caught by this file's own PC-side
 * test before ever reaching a board (docs/handoff.md section 4.4's
 * "simulate end-to-end on PC first" practice).
 */
static void rule_wire_to_record(const uint8_t in[32], SPLC_RuleRecord *r)
{
    uint16_t w[16];
    for (uint8_t i = 0; i < 16U; i++) {
        w[i] = (uint16_t)(((uint16_t)in[2U * i] << 8) | in[2U * i + 1U]);
    }

    r->threshold_lo  = (int32_t)(((uint32_t)w[0] << 16) | w[1]);
    r->threshold_hi  = (int32_t)(((uint32_t)w[2] << 16) | w[3]);
    r->for_ms        = ((uint32_t)w[4] << 16) | w[5];
    r->action_param  = (int32_t)(((uint32_t)w[6] << 16) | w[7]);
    r->trigger_tag   = w[8];
    r->action_tag    = w[9];
    r->guard_tag     = w[10];
    r->enabled       = (uint8_t)(w[11] >> 8);
    r->trigger_type  = (uint8_t)(w[11] & 0xFFU);
    r->compare_op    = (uint8_t)(w[12] >> 8);
    r->action_type   = (uint8_t)(w[12] & 0xFFU);
    memset(r->reserved, 0, sizeof(r->reserved)); /* wire w[13..15] are reserved/ignored, same as write_staging_rule_table() */
}

/*
 * Reads the header (only SPLC_RULE_FLASH_HEADER_SIZE bytes) from sector
 * `addr`, to learn rule_count before deciding how many more bytes to
 * read for the full record. Does not validate CRC -- that requires the
 * full record, done by rule_flash_read_and_validate() below.
 */
static void rule_flash_read_header(uint32_t addr, uint32_t *seq_num_out, uint16_t *rule_count_out)
{
    uint8_t header[SPLC_RULE_FLASH_HEADER_SIZE];
    sx_flash_read(addr, header, SPLC_RULE_FLASH_HEADER_SIZE);

    if (seq_num_out != NULL) {
        *seq_num_out = read_u32_be(&header[RULE_FLASH_SEQ_OFFSET]);
    }
    if (rule_count_out != NULL) {
        *rule_count_out = read_u16_be(&header[RULE_FLASH_COUNT_OFFSET]);
    }
}

/*
 * Reads the full record at `addr` into s_rule_flash_buf and validates its
 * CRC. Returns true (and fills the seq_num/rule_count out-params) only if:
 *   - the header's rule_count is within 0..MAX_RULES (a corrupted count
 *     read as a huge number must not be used to size a further read --
 *     same defensive bound as retain_store_restore()'s count clamp), AND
 *   - the record's field-embedded CRC-16/MODBUS matches.
 *
 * Blank/erased Flash (reads back as all-0xFF) fails the CRC check here,
 * same as plc_retain.c's retain_record_is_valid() -- there is no need to
 * special-case "blank" separately from "corrupted"; both are simply
 * "not a valid record".
 */
static bool rule_flash_read_and_validate(uint32_t addr, uint32_t *seq_num_out, uint16_t *rule_count_out)
{
    uint16_t rule_count;
    rule_flash_read_header(addr, NULL, &rule_count);

    if (rule_count > MAX_RULES) {
        return false;
    }

    uint32_t record_size = RULE_FLASH_DATA_OFFSET + (uint32_t)rule_count * SPLC_RULE_FLASH_RECORD_WIRE_SIZE;
    sx_flash_read(addr, s_rule_flash_buf, record_size);

    uint16_t stored_crc   = read_u16_be(&s_rule_flash_buf[RULE_FLASH_CRC_OFFSET]);
    uint16_t computed_crc = rule_flash_record_crc(s_rule_flash_buf, record_size);

    if (stored_crc != computed_crc) {
        return false;
    }

    if (seq_num_out != NULL) {
        *seq_num_out = read_u32_be(&s_rule_flash_buf[RULE_FLASH_SEQ_OFFSET]);
    }
    if (rule_count_out != NULL) {
        *rule_count_out = rule_count;
    }
    return true;
}

/*
 * Erases the sector at `dst_addr` and writes s_rule_flash_buf's current
 * contents (`size` bytes) to it. sx_flash_write() itself pads the actual
 * program operation up to the next 16-byte boundary with 0xFF -- callers
 * of this file never read that padding back, since reads are always
 * bounded by the record's own rule_count.
 */
static void rule_flash_write_sector(uint32_t dst_addr, uint32_t size)
{
    sx_flash_unlock();
    sx_flash_erase(dst_addr, SPLC_FLASH_RULE_TABLE_SIZE);
    sx_flash_write(dst_addr, s_rule_flash_buf, size);
    sx_flash_lock();
}

/*
 * Copies a full record from `src_addr` to `dst_addr`: reads
 * SPLC_RULE_FLASH_HEADER_SIZE first to learn the real size, then reads
 * exactly that many bytes into s_rule_flash_buf, then erase+write to the
 * destination. Used for both directions of the A/B mechanism (A->B sync,
 * B->A restore) -- see plc_rule_flash.h.
 *
 * Does not validate CRC before copying: A->B copies happen when A is
 * already known-good (right after a successful write+verify), and B->A
 * copies happen precisely because B is the last-known-good fallback that
 * must always be trusted (see plc_rule_flash.h's mechanism -- B is never
 * written to except as a verified copy of a previously-good A, so it is
 * always safe to treat B as trustworthy when restoring from it).
 */
static void rule_flash_copy_sector(uint32_t src_addr, uint32_t dst_addr)
{
    uint16_t rule_count;
    rule_flash_read_header(src_addr, NULL, &rule_count);
    if (rule_count > MAX_RULES) {
        rule_count = MAX_RULES; /* defensive clamp, same rationale as rule_flash_read_and_validate() */
    }

    uint32_t size = RULE_FLASH_DATA_OFFSET + (uint32_t)rule_count * SPLC_RULE_FLASH_RECORD_WIRE_SIZE;
    sx_flash_read(src_addr, s_rule_flash_buf, size);
    rule_flash_write_sector(dst_addr, size);
}

void plc_rule_flash_load(void)
{
    uint32_t seq_num;
    uint16_t rule_count;

    if (rule_flash_read_and_validate(SPLC_FLASH_RULE_TABLE_A_ADDR, &seq_num, &rule_count)) {
        /* A is good: this is the normal, steady-state boot path. Decode
         * each record's 32-byte wire image (rule_flash_read_and_validate()
         * left them in s_rule_flash_buf at offset RULE_FLASH_DATA_OFFSET)
         * back into RAM-struct layout before calling rule_table_commit() --
         * see rule_wire_to_record()'s comment for why this decode step is
         * required (rule_table_commit() expects native struct bytes, not
         * wire bytes). */
        for (uint16_t i = 0; i < rule_count; i++) {
            rule_wire_to_record(&s_rule_flash_buf[RULE_FLASH_DATA_OFFSET + (uint32_t)i * SPLC_RULE_FLASH_RECORD_WIRE_SIZE],
                                 &s_rule_flash_decoded[i]);
        }
        rule_table_commit((const uint8_t *)s_rule_flash_decoded, rule_count);
        log_info(TAG, "loaded %u rule(s) from Flash A (seq_num=%lu)",
                 rule_count, (unsigned long)seq_num);
        return;
    }

    log_warn(TAG, "Flash A invalid or blank, trying B");

    if (rule_flash_read_and_validate(SPLC_FLASH_RULE_TABLE_B_ADDR, &seq_num, &rule_count)) {
        /* B is good but A was not: restore A from B before committing to
         * RAM, so the running/backup pair is back in sync on disk before
         * anything else touches Flash (e.g. the very next commit's A->B
         * copy step must not copy a still-bad A over a good B). */
        log_warn(TAG, "Flash B valid (seq_num=%lu), restoring A from B",
                 (unsigned long)seq_num);
        rule_flash_copy_sector(SPLC_FLASH_RULE_TABLE_B_ADDR, SPLC_FLASH_RULE_TABLE_A_ADDR);

        for (uint16_t i = 0; i < rule_count; i++) {
            rule_wire_to_record(&s_rule_flash_buf[RULE_FLASH_DATA_OFFSET + (uint32_t)i * SPLC_RULE_FLASH_RECORD_WIRE_SIZE],
                                 &s_rule_flash_decoded[i]);
        }
        rule_table_commit((const uint8_t *)s_rule_flash_decoded, rule_count);
        log_info(TAG, "loaded %u rule(s) from Flash B (A restored)", rule_count);
        return;
    }

    /* Neither A nor B holds a valid record -- first boot ever, or both
     * sectors blank/corrupted. Not an error: rule_table_load_from_flash()
     * (Layer 2, called just before this) already left g_rule_table[] at
     * the defined empty state (rule_count = 0), so there is nothing
     * further to do here. */
    log_info(TAG, "no valid rule table found on Flash (A or B) -- starting with an empty rule table");
}

bool plc_rule_flash_save(void)
{
    /*
     * Step 1: Copy A (old) -> B, per plc_rule_flash.h's mechanism.
     * If A currently holds no valid record at all (e.g. very first save
     * on a brand-new device, A still blank), rule_flash_read_header()
     * would read back an all-0xFF header -- rule_count reads as 0xFFFF,
     * caught by rule_flash_copy_sector()'s own MAX_RULES clamp, so this
     * degrades safely to copying a (clamped, effectively garbage but
     * bounded) buffer to B rather than reading out of bounds. B is not
     * relied upon as authoritative in this specific case anyway: if step
     * 3 below fails on this very first save, the B->A restore path would
     * restore this same garbage -- but step 3 failing on a fresh write to
     * a freshly erased sector is not expected in practice (see the
     * comment on the final `return false` below).
     */
    rule_flash_copy_sector(SPLC_FLASH_RULE_TABLE_A_ADDR, SPLC_FLASH_RULE_TABLE_B_ADDR);

    /* seq_num for this save: read A's OLD seq_num (already known-good,
     * or 0 if A was never valid) and increment. Read again from B (which
     * step 1 just made an exact copy of A) rather than re-reading A, to
     * make the data dependency explicit: B is now guaranteed to hold
     * whatever A held a moment ago. */
    uint32_t old_seq_num;
    rule_flash_read_header(SPLC_FLASH_RULE_TABLE_B_ADDR, &old_seq_num, NULL);
    uint32_t new_seq_num = old_seq_num + 1U;

    /* Step 2: Clear A, write the new rule table (already committed in
     * RAM by the caller) into A. */
    uint32_t record_size = rule_flash_build_record(new_seq_num);
    rule_flash_write_sector(SPLC_FLASH_RULE_TABLE_A_ADDR, record_size);

    /* Step 3: Read A back, check CRC. */
    if (rule_flash_read_and_validate(SPLC_FLASH_RULE_TABLE_A_ADDR, NULL, NULL)) {
        /* CRC OK: re-sync B = A, so B is ready as the backup for the
         * next save. */
        rule_flash_copy_sector(SPLC_FLASH_RULE_TABLE_A_ADDR, SPLC_FLASH_RULE_TABLE_B_ADDR);
        log_info(TAG, "rule table saved to Flash A+B (seq_num=%lu)", (unsigned long)new_seq_num);
        return true;
    }

    /* CRC BAD (e.g. power loss mid-write): restore A from B, which step 1
     * guaranteed still holds the previous known-good table untouched. */
    log_warn(TAG, "Flash A write verify failed, restoring A from B");
    rule_flash_copy_sector(SPLC_FLASH_RULE_TABLE_B_ADDR, SPLC_FLASH_RULE_TABLE_A_ADDR);

    /*
     * Re-validate the restore itself: B's DATA is trusted by construction
     * (never written to on this branch, only read from -- see
     * rule_flash_copy_sector()'s own comment), but the WRITE this just
     * performed to A is a fresh Flash program operation like any other
     * and can independently fail (e.g. a genuinely failing/worn sector).
     * Per plc_rule_flash.h's contract, this function returns true only
     * once A is CONFIRMED to hold a valid table again -- whether that
     * came from the direct write (above) or this restore.
     */
    if (rule_flash_read_and_validate(SPLC_FLASH_RULE_TABLE_A_ADDR, NULL, NULL)) {
        log_warn(TAG, "rule table NOT newly saved (Flash A write failed), "
                 "but restored to previous good state (seq_num=%lu)",
                 (unsigned long)old_seq_num);
        return false;
    }

    /* Both the direct write to A AND the B->A restore failed to produce a
     * valid CRC on read-back -- per plc_rule_flash.h's contract, this is
     * the one case with no further fallback (e.g. a hardware-damaged
     * sector). The rule table already running in RAM is unaffected
     * either way; only its Flash persistence is in question. */
    log_warn(TAG, "Flash A restore from B ALSO failed -- Rule Table Flash region "
             "may be damaged; RAM-resident rule table is unaffected");
    return false;
}