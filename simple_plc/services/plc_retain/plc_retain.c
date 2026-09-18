/*
 * plc_retain.c - Layer 3 (PLC Application Services)
 *
 * Implementation of the retain scheme declared in plc_retain.h. See that
 * file's header comment, app/splc_flash_define.h (Flash layout/size
 * constants), and docs/SimplePLC_RuleStruct_MCU_Spec_v0.1.md section 7
 * for the full design this implements.
 *
 * Record wire layout (SPLC_RETAIN_RECORD_SIZE = 200 bytes, per
 * app/splc_flash_define.h):
 *   offset 0..3   uint32_t seq_num
 *   offset 4..5   uint16_t count       (number of valid entries that follow)
 *   offset 6..7   uint16_t crc16       (CRC-16/MODBUS over seq_num+count+entries,
 *                                       with crc16 field itself treated as 0)
 *   offset 8..    count x { uint16_t tag_index; int32_t value; }  (6 bytes each)
 *
 * A record always reserves space for SPLC_RETAIN_TAG_COUNT (32) entries
 * on Flash (fixed SPLC_RETAIN_RECORD_SIZE), even though count may be
 * smaller in principle -- this implementation always writes exactly
 * SPLC_RETAIN_TAG_COUNT entries (every TAG_VREG_RETAIN tag, in fixed
 * index order), so count is always SPLC_RETAIN_TAG_COUNT in practice.
 * The field still exists on the wire for forward compatibility and so
 * retain_store_restore() has an explicit, self-describing entry count
 * to loop over rather than assuming the fixed constant forever.
 */

#include "plc_retain.h"

#include <string.h>

#include "splc_flash_define.h"
#include "sx_flash.h"
#include "sx_time.h"
#include "nanomodbus.h"
#include "plc_tag.h"
#include "plc_tag_def.h"

#define RETAIN_RECORD_SEQ_OFFSET    0U
#define RETAIN_RECORD_COUNT_OFFSET  4U
#define RETAIN_RECORD_CRC_OFFSET    6U
#define RETAIN_RECORD_ENTRIES_OFFSET 8U

/* Sentinel meaning "no valid record has been found yet" while scanning --
 * a real seq_num can be 0 (the very first record ever written), so this
 * cannot double as "not found"; a separate found-flag is used instead
 * (see retain_store_restore()). */
static uint32_t s_next_seq_num = 0;   /* seq_num to use for the NEXT write */
static uint32_t s_write_sector = 0;   /* sector index (0..SPLC_FLASH_RETAIN_SECTOR_COUNT-1) currently being appended to */
static uint32_t s_write_slot   = 0;   /* record slot within s_write_sector for the NEXT write (0..SPLC_RETAIN_RECORDS_PER_SECTOR-1) */
static bool     s_write_pos_known = false; /* true once retain_store_restore() has established where to write next */

static uint32_t s_last_snapshot_tick_ms = 0;

/*
 * Address of sector `sector_idx` (0..SPLC_FLASH_RETAIN_SECTOR_COUNT-1)
 * within the retain region.
 */
static uint32_t retain_sector_addr(uint32_t sector_idx)
{
    return SPLC_FLASH_RETAIN_BASE_ADDR + sector_idx * SPLC_FLASH_SECTOR_SIZE;
}

/*
 * Address of record slot `slot_idx` (0..SPLC_RETAIN_RECORDS_PER_SECTOR-1)
 * within sector `sector_idx`.
 */
static uint32_t retain_record_addr(uint32_t sector_idx, uint32_t slot_idx)
{
    return retain_sector_addr(sector_idx) + slot_idx * SPLC_RETAIN_RECORD_SIZE;
}

/*
 * Computes the CRC-16/MODBUS that should be stored in a record's crc16
 * field: over the whole record EXCEPT the crc16 field itself, with that
 * field's 2 bytes treated as 0 (checksummed-with-self-zeroed is the
 * standard way to make a field-embedded CRC self-consistent -- avoids
 * needing to exclude a byte range from the middle of the buffer).
 *
 * raw: pointer to a full SPLC_RETAIN_RECORD_SIZE-byte record buffer.
 *      Only raw[RETAIN_RECORD_CRC_OFFSET..+1] is temporarily zeroed
 *      (and restored) by this function; the rest of raw is read-only.
 *
 * nanoMODBUS's nmbs_crc_calc() returns its result byte-swapped for RTU
 * wire order (low byte first, ready to transmit) -- this function swaps
 * it back to a plain arithmetic uint16_t before returning, since this
 * record's crc16 field is stored via write_u16_be() like every other
 * multi-byte field in the record, not transmitted as a raw RTU frame.
 */
static uint16_t retain_record_crc(uint8_t *raw)
{
    uint8_t saved0 = raw[RETAIN_RECORD_CRC_OFFSET];
    uint8_t saved1 = raw[RETAIN_RECORD_CRC_OFFSET + 1];

    raw[RETAIN_RECORD_CRC_OFFSET]     = 0;
    raw[RETAIN_RECORD_CRC_OFFSET + 1] = 0;

    uint16_t wire_order_crc = nmbs_crc_calc(raw, SPLC_RETAIN_RECORD_SIZE, NULL);
    uint16_t crc = (uint16_t)((wire_order_crc << 8) | (wire_order_crc >> 8));

    raw[RETAIN_RECORD_CRC_OFFSET]     = saved0;
    raw[RETAIN_RECORD_CRC_OFFSET + 1] = saved1;

    return crc;
}

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

static int32_t read_i32_be(const uint8_t *p)
{
    return (int32_t)read_u32_be(p);
}

static void write_i32_be(uint8_t *p, int32_t v)
{
    write_u32_be(p, (uint32_t)v);
}

/*
 * Reads and validates the record at (sector_idx, slot_idx). Returns true
 * and fills *seq_num_out if the record has a matching CRC-16/MODBUS (a
 * genuine, previously-written record); returns false for blank/erased
 * Flash (reads back as all-0xFF, which will not match its own CRC) or
 * any other corruption.
 */
static bool retain_record_is_valid(uint32_t sector_idx, uint32_t slot_idx, uint32_t *seq_num_out)
{
    uint8_t raw[SPLC_RETAIN_RECORD_SIZE];
    sx_flash_read(retain_record_addr(sector_idx, slot_idx), raw, SPLC_RETAIN_RECORD_SIZE);

    uint16_t stored_crc   = read_u16_be(&raw[RETAIN_RECORD_CRC_OFFSET]);
    uint16_t computed_crc = retain_record_crc(raw);

    if (stored_crc != computed_crc) {
        return false;
    }

    if (seq_num_out != NULL) {
        *seq_num_out = read_u32_be(&raw[RETAIN_RECORD_SEQ_OFFSET]);
    }
    return true;
}

void retain_store_restore(void)
{
    bool     found_any        = false;
    uint32_t best_seq          = 0;
    uint32_t best_sector       = 0;
    uint32_t best_slot         = 0;

    /* Scan every record slot in every retain sector for the one with the
     * highest seq_num and a valid CRC -- per
     * docs/SimplePLC_RuleStruct_MCU_Spec_v0.1.md section 7.2, no
     * separate write-position pointer is persisted; it is always
     * re-derived at boot by this scan. */
    for (uint32_t sector = 0; sector < SPLC_FLASH_RETAIN_SECTOR_COUNT; sector++) {
        for (uint32_t slot = 0; slot < SPLC_RETAIN_RECORDS_PER_SECTOR; slot++) {
            uint32_t seq;
            if (!retain_record_is_valid(sector, slot, &seq)) {
                continue;
            }
            if (!found_any || seq > best_seq) {
                found_any  = true;
                best_seq   = seq;
                best_sector = sector;
                best_slot   = slot;
            }
        }
    }

    if (found_any) {
        uint8_t raw[SPLC_RETAIN_RECORD_SIZE];
        sx_flash_read(retain_record_addr(best_sector, best_slot), raw, SPLC_RETAIN_RECORD_SIZE);

        uint16_t count = read_u16_be(&raw[RETAIN_RECORD_COUNT_OFFSET]);
        if (count > SPLC_RETAIN_TAG_COUNT) {
            /* Defensive: a corrupted-but-CRC-matching count (should not
             * happen, CRC covers this field too) must not walk past the
             * fixed-size entries area below. */
            count = SPLC_RETAIN_TAG_COUNT;
        }

        for (uint16_t i = 0; i < count; i++) {
            const uint8_t *entry = &raw[RETAIN_RECORD_ENTRIES_OFFSET + i * 6U];
            uint16_t tag_idx = read_u16_be(&entry[0]);
            int32_t  value   = read_i32_be(&entry[2]);

            if (tag_idx < MAX_TAGS && tag_get_kind(tag_idx) == TAG_VREG_RETAIN) {
                tag_write(tag_idx, value);
            }
            /* A tag_idx that is out of range or no longer TAG_VREG_RETAIN
             * (e.g. after a firmware update changed the tag map) is
             * silently skipped rather than treated as a fatal error --
             * consistent with tag_write()/tag_read()'s own bounds
             * checking elsewhere in the system. */
        }

        /* Next write continues right after the record just restored,
         * in the same sector -- the scan already found this is the
         * newest valid record, so the next free (blank) slot after it
         * in write order is where retain_snapshot_write() should append
         * next. Slot layout is written strictly in increasing slot
         * order within a sector (see retain_snapshot_write()), so the
         * next slot index is simply best_slot + 1 (wrapping to a new
         * sector below if that runs off the end of this one). */
        s_next_seq_num = best_seq + 1U;
        s_write_sector = best_sector;
        s_write_slot   = best_slot + 1U;
        if (s_write_slot >= SPLC_RETAIN_RECORDS_PER_SECTOR) {
            s_write_sector = (best_sector + 1U) % SPLC_FLASH_RETAIN_SECTOR_COUNT;
            s_write_slot   = 0U;
        }
        s_write_pos_known = true;
    } else {
        /* First boot ever, or the whole region is blank/corrupted: start
         * fresh at the very first sector/slot. Every TAG_VREG_RETAIN tag
         * is left at its power-on value (0) -- per
         * docs/SimplePLC_RuleStruct_MCU_Spec_v0.1.md section 7.2, this is
         * not treated as an error. */
        s_next_seq_num = 0U;
        s_write_sector = 0U;
        s_write_slot   = 0U;
        s_write_pos_known = true;
    }

    s_last_snapshot_tick_ms = sx_get_tick_ms();
}

void retain_snapshot_write(void)
{
    if (!s_write_pos_known) {
        /* Defensive: retain_store_restore() must run first (Layer 4's
         * plc_engine_init() ordering guarantees this in practice -- see
         * plc_retain.h). Refusing to write with an unknown position
         * avoids silently clobbering a slot whose validity hasn't been
         * established yet. */
        return;
    }

    if (s_write_slot >= SPLC_RETAIN_RECORDS_PER_SECTOR) {
        /* Current sector is full: rotate to the next sector in the ring
         * and erase it before writing -- per
         * docs/SimplePLC_RuleStruct_MCU_Spec_v0.1.md section 7.2, the
         * next sector in rotation is always the oldest, so it is always
         * safe to erase. */
        s_write_sector = (s_write_sector + 1U) % SPLC_FLASH_RETAIN_SECTOR_COUNT;
        s_write_slot   = 0U;
        sx_flash_erase(retain_sector_addr(s_write_sector), SPLC_FLASH_SECTOR_SIZE);
    }

    uint8_t raw[SPLC_RETAIN_RECORD_SIZE];
    memset(raw, 0, sizeof(raw));

    write_u32_be(&raw[RETAIN_RECORD_SEQ_OFFSET], s_next_seq_num);
    write_u16_be(&raw[RETAIN_RECORD_COUNT_OFFSET], (uint16_t)SPLC_RETAIN_TAG_COUNT);

    static const uint16_t retain_tag_indices[SPLC_RETAIN_TAG_COUNT] = {
        TAG_VREG_R0,  TAG_VREG_R1,  TAG_VREG_R2,  TAG_VREG_R3,
        TAG_VREG_R4,  TAG_VREG_R5,  TAG_VREG_R6,  TAG_VREG_R7,
        TAG_VREG_R8,  TAG_VREG_R9,  TAG_VREG_R10, TAG_VREG_R11,
        TAG_VREG_R12, TAG_VREG_R13, TAG_VREG_R14, TAG_VREG_R15,
        TAG_VREG_R16, TAG_VREG_R17, TAG_VREG_R18, TAG_VREG_R19,
        TAG_VREG_R20, TAG_VREG_R21, TAG_VREG_R22, TAG_VREG_R23,
        TAG_VREG_R24, TAG_VREG_R25, TAG_VREG_R26, TAG_VREG_R27,
        TAG_VREG_R28, TAG_VREG_R29, TAG_VREG_R30, TAG_VREG_R31,
    };

    for (uint16_t i = 0; i < SPLC_RETAIN_TAG_COUNT; i++) {
        uint8_t *entry = &raw[RETAIN_RECORD_ENTRIES_OFFSET + i * 6U];
        write_u16_be(&entry[0], retain_tag_indices[i]);
        write_i32_be(&entry[2], tag_read(retain_tag_indices[i]));
    }

    uint16_t crc = retain_record_crc(raw);
    write_u16_be(&raw[RETAIN_RECORD_CRC_OFFSET], crc);

    uint32_t addr = retain_record_addr(s_write_sector, s_write_slot);

    sx_flash_unlock();
    sx_flash_write(addr, raw, SPLC_RETAIN_RECORD_SIZE);
    sx_flash_lock();

    s_next_seq_num++;
    s_write_slot++;
}

void retain_service(void)
{
    uint32_t now = sx_get_tick_ms();

    /* Unsigned subtraction handles tick wraparound correctly as long as
     * the actual elapsed time never exceeds UINT32_MAX ms (~49.7 days) --
     * true for any period this project uses. */
    if ((now - s_last_snapshot_tick_ms) >= RETAIN_SNAPSHOT_PERIOD_MS) {
        retain_snapshot_write();
        s_last_snapshot_tick_ms = now;
    }
}