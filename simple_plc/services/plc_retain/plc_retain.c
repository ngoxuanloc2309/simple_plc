/*
 * plc_retain.c - Layer 3 (PLC Application Services)
 *
 * Implementation of the retain scheme declared in plc_retain.h. See that
 * file's header comment, app/splc_flash_define.h (Flash layout/size
 * constants), and docs/SimplePLC_RuleStruct_MCU_Spec_v0.1.md section 7
 * for the full design this implements.
 *
 * Record wire layout (SPLC_RETAIN_RECORD_SIZE = 208 bytes -- 200 bytes
 * of real header+entry data, rounded up to the next 16-byte/quad-word
 * multiple so every record slot lands on a Flash-program-legal address;
 * see splc_flash_define.h's SPLC_RETAIN_RECORD_SIZE comment for the
 * real-hardware bug this fixes -- per app/splc_flash_define.h):
 *   offset 0..3   uint32_t seq_num
 *   offset 4..5   uint16_t count       (number of valid entries that follow)
 *   offset 6..7   uint16_t crc16       (CRC-16/MODBUS over the WHOLE
 *                                       SPLC_RETAIN_RECORD_SIZE-byte record,
 *                                       including the 8 bytes of trailing
 *                                       alignment padding, with the crc16
 *                                       field itself treated as 0)
 *   offset 8..207 count x { uint16_t tag_index; int32_t value; }  (6 bytes
 *                                       each, count is always
 *                                       SPLC_RETAIN_TAG_COUNT == 32 in
 *                                       practice -- see below), followed
 *                                       by 8 bytes of unused padding
 *                                       (written as 0 by
 *                                       retain_snapshot_write()'s
 *                                       memset(), never read back by any
 *                                       path in this file)
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
 * Flash-operation guard, shared by every main-loop Flash operation (this
 * file's own writes/erases and plc_rule_flash.c's save, which brackets its
 * erase+write with retain_flash_op_begin()/end()). The PVD interrupt can
 * fire at any instruction; if it fired while the main loop was inside
 * HAL_FLASH_Program()/HAL_FLASHEx_Erase(), the HAL's process lock would
 * make the interrupt's own call fail with BUSY -- and the interrupt must
 * not touch s_write_sector/s_write_slot while the main loop is part-way
 * through updating them. While the depth is non-zero the interrupt does
 * nothing except raise s_emergency_pending; retain_service() then does the
 * write from the main loop once the Flash is free.
 *
 * A plain volatile counter is enough on this single-core MCU: the
 * interrupt never changes the depth by a net amount (it only runs when the
 * depth is 0 and leaves it 0), so the main loop's non-atomic increment
 * cannot be corrupted by it.
 */
static volatile uint32_t s_flash_op_depth       = 0U;
static volatile bool     s_emergency_pending    = false;
static volatile bool     s_emergency_seen       = false; /* an emergency write has been attempted since boot */
static volatile uint32_t s_last_emergency_tick_ms = 0U;

/*
 * Values of the 32 retain tags as stored in the newest valid Flash record
 * (or as loaded at boot). Lets the periodic and emergency paths skip a
 * write -- and the Flash wear -- when nothing changed since the last save.
 */
static int32_t s_saved_values[SPLC_RETAIN_TAG_COUNT];
static bool    s_saved_valid = false;

/*
 * TAG_VREG_R0..31 used to be fixed #define's (core/plc_tag/plc_tag_def.h,
 * now removed) that this array could initialize at compile time. Since a
 * board's VREG_RETAIN group now starts at a board-dependent offset (see
 * tag_vreg_retain_base_index(), plc_tag.h), this is computed once, lazily,
 * on first use instead -- cheap (32 additions) and avoids repeating that
 * work every retain_snapshot_write() call (retain_service() runs it on a
 * timer, not once).
 */
static uint16_t s_retain_tag_indices[SPLC_RETAIN_TAG_COUNT];
static bool     s_retain_tag_indices_ready = false;

static void retain_tag_indices_init(void)
{
    if (s_retain_tag_indices_ready) {
        return;
    }
    uint16_t base = tag_vreg_retain_base_index();
    for (uint16_t i = 0; i < SPLC_RETAIN_TAG_COUNT; i++) {
        s_retain_tag_indices[i] = (uint16_t)(base + i);
    }
    s_retain_tag_indices_ready = true;
}

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


void retain_flash_op_begin(void)
{
    s_flash_op_depth++;
}

void retain_flash_op_end(void)
{
    if (s_flash_op_depth != 0U) {
        s_flash_op_depth--;
    }
}

/* True if the whole record slot still reads as erased (all 0xFF), i.e. it
 * can be programmed. A slot that holds a partly programmed record (power
 * lost mid-write) is NOT blank and cannot be reused until its sector is
 * erased. Reads through sx_flash_read() like every other read here (plain
 * loads, no unlock: safe from the PVD interrupt); 16 bytes at a time to
 * keep the interrupt's stack use small. */
static bool retain_slot_is_blank(uint32_t sector_idx, uint32_t slot_idx)
{
    uint32_t addr = retain_record_addr(sector_idx, slot_idx);
    uint8_t  chunk[16];

    for (uint32_t off = 0; off < SPLC_RETAIN_RECORD_SIZE; off += sizeof(chunk)) {
        sx_flash_read(addr + off, chunk, sizeof(chunk));
        for (uint32_t i = 0; i < sizeof(chunk); i++) {
            if (chunk[i] != 0xFFU) {
                return false;
            }
        }
    }
    return true;
}

static bool retain_sector_is_blank(uint32_t sector_idx)
{
    for (uint32_t slot = 0; slot < SPLC_RETAIN_RECORDS_PER_SECTOR; slot++) {
        if (!retain_slot_is_blank(sector_idx, slot)) {
            return false;
        }
    }
    return true;
}

/* Erases one retain sector. Flash must be unlocked for an erase exactly as
 * for a program (plc_rule_flash.c does unlock -> erase -> write -> lock). */
static void retain_erase_sector(uint32_t sector_idx)
{
    sx_flash_unlock();
    sx_flash_erase(retain_sector_addr(sector_idx), SPLC_FLASH_SECTOR_SIZE);
    sx_flash_lock();
}

/* Moves the write position to slot 0 of the next sector in the ring and
 * erases it. Main loop only (erase is slow and not allowed in the PVD
 * interrupt). The next sector in rotation always holds the OLDEST
 * records, so it is always safe to erase. */
static void retain_rotate_and_erase(void)
{
    s_write_sector = (s_write_sector + 1U) % SPLC_FLASH_RETAIN_SECTOR_COUNT;
    s_write_slot   = 0U;
    retain_erase_sector(s_write_sector);
}

static bool retain_values_changed(void)
{
    if (!s_saved_valid) {
        return true;
    }
    retain_tag_indices_init();
    for (uint16_t i = 0; i < SPLC_RETAIN_TAG_COUNT; i++) {
        if (tag_read(s_retain_tag_indices[i]) != s_saved_values[i]) {
            return true;
        }
    }
    return false;
}

/*
 * Makes the write position safe to program, at boot (main loop, so an
 * erase is allowed). Two things can leave it unusable:
 *   1. The slot right after the newest record holds a partly programmed
 *      record (power lost during a write, which the emergency path makes
 *      more likely): skip forward over non-blank slots.
 *   2. The newest record filled its sector and power was lost before the
 *      next sector was pre-erased (see retain_write_record()): the next
 *      sector still holds old records and must be erased now, so that the
 *      first write after boot -- possibly the PVD one -- needs no erase.
 */
static void retain_prepare_write_position(void)
{
    while (s_write_slot < SPLC_RETAIN_RECORDS_PER_SECTOR &&
           !retain_slot_is_blank(s_write_sector, s_write_slot)) {
        s_write_slot++;
    }

    if (s_write_slot >= SPLC_RETAIN_RECORDS_PER_SECTOR) {
        s_write_sector = (s_write_sector + 1U) % SPLC_FLASH_RETAIN_SECTOR_COUNT;
        s_write_slot   = 0U;
        if (!retain_sector_is_blank(s_write_sector)) {
            retain_erase_sector(s_write_sector);
        }
    }
}

void retain_store_restore(void)
{
    /* Must run after tag_table_load_from_flash() (plc_engine_init()'s call
     * order guarantees this) so tag_vreg_retain_base_index() already
     * reflects this board's real layout. */
    retain_tag_indices_init();

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
        s_write_pos_known = true;
        retain_prepare_write_position();
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
        retain_prepare_write_position();
    }

    /* Whatever the tags hold now is what Flash holds (restored values, or
     * the all-zero power-on state when there was no record): nothing to
     * save until a tag changes. */
    for (uint16_t i = 0; i < SPLC_RETAIN_TAG_COUNT; i++) {
        s_saved_values[i] = tag_read(s_retain_tag_indices[i]);
    }
    s_saved_valid = true;

    s_last_snapshot_tick_ms = sx_get_tick_ms();
}

/*
 * Writes one record with the current retain tag values. Shared by the
 * main-loop paths (in_isr == false) and the PVD interrupt (in_isr == true);
 * the caller has already made sure no other Flash operation is running
 * (retain_flash_op_begin(), or depth 0 for the interrupt).
 *
 * Interrupt path differences: never erases (a full sector returns false
 * and the caller defers to the main loop), programs with
 * sx_flash_write_quiet() (no logging), and does not pre-erase afterwards.
 *
 * Returns true only if the record was read back with a valid CRC and the
 * expected seq_num.
 */
static bool retain_write_record(bool in_isr)
{
    if (!s_write_pos_known) {
        /* Defensive: retain_store_restore() must run first (plc_engine_init()
         * guarantees this). Refusing to write with an unknown position
         * avoids clobbering a slot whose validity hasn't been established. */
        return false;
    }

    retain_tag_indices_init();

    /* Find a programmable slot. A slot that is not blank (earlier failed or
     * interrupted write) is consumed. The main loop may rotate into the
     * next sector, but at most once around the ring so a sector that
     * refuses to erase cannot spin here forever. */
    uint32_t rotations = 0U;
    for (;;) {
        if (s_write_slot >= SPLC_RETAIN_RECORDS_PER_SECTOR) {
            if (in_isr || rotations >= SPLC_FLASH_RETAIN_SECTOR_COUNT) {
                return false;
            }
            retain_rotate_and_erase();
            rotations++;
        }
        if (retain_slot_is_blank(s_write_sector, s_write_slot)) {
            break;
        }
        s_write_slot++;
    }

    int32_t snap[SPLC_RETAIN_TAG_COUNT];
    uint8_t raw[SPLC_RETAIN_RECORD_SIZE];
    memset(raw, 0, sizeof(raw));

    write_u32_be(&raw[RETAIN_RECORD_SEQ_OFFSET], s_next_seq_num);
    write_u16_be(&raw[RETAIN_RECORD_COUNT_OFFSET], (uint16_t)SPLC_RETAIN_TAG_COUNT);

    for (uint16_t i = 0; i < SPLC_RETAIN_TAG_COUNT; i++) {
        uint8_t *entry = &raw[RETAIN_RECORD_ENTRIES_OFFSET + i * 6U];
        snap[i] = tag_read(s_retain_tag_indices[i]);
        write_u16_be(&entry[0], s_retain_tag_indices[i]);
        write_i32_be(&entry[2], snap[i]);
    }

    uint16_t crc = retain_record_crc(raw);
    write_u16_be(&raw[RETAIN_RECORD_CRC_OFFSET], crc);

    uint32_t addr = retain_record_addr(s_write_sector, s_write_slot);

    sx_flash_unlock();
    if (in_isr) {
        (void)sx_flash_write_quiet(addr, raw, SPLC_RETAIN_RECORD_SIZE);
    } else {
        sx_flash_write(addr, raw, SPLC_RETAIN_RECORD_SIZE);
    }
    sx_flash_lock();

    const uint32_t written_seq  = s_next_seq_num;
    const uint32_t written_sect = s_write_sector;
    const uint32_t written_slot = s_write_slot;

    /* The slot is consumed even if the write failed: a partly programmed
     * slot cannot be reused. */
    s_next_seq_num++;
    s_write_slot++;

    /* Read back and verify (Wire Contract section 7, step 3). */
    uint32_t seq_back = 0U;
    bool verified = retain_record_is_valid(written_sect, written_slot, &seq_back) &&
                    (seq_back == written_seq);

    if (verified) {
        memcpy(s_saved_values, snap, sizeof(s_saved_values));
        s_saved_valid = true;
    }

    /* Pre-erase: that record filled its sector, so erase the next one NOW
     * (main loop, no deadline) instead of lazily at the next write. The
     * next write may be the PVD one, which must not erase. The record just
     * written is in the sector being left, so erasing the oldest sector
     * never touches the newest valid record. */
    if (!in_isr && s_write_slot >= SPLC_RETAIN_RECORDS_PER_SECTOR) {
        retain_rotate_and_erase();
    }

    return verified;
}

bool retain_snapshot_write(void)
{
    retain_flash_op_begin();
    bool ok = retain_write_record(false);
    retain_flash_op_end();
    return ok;
}

void retain_emergency_snapshot(void)
{
    /* Interrupt context. No logging, no erase, no blocking beyond the
     * quad-word programming of one record. */
    if (!s_write_pos_known) {
        return;
    }

    uint32_t now = sx_get_tick_ms();

    /* The detector can chatter around its threshold; one emergency write
     * per interval is enough and bounds the Flash wear. */
    if (s_emergency_seen &&
        (now - s_last_emergency_tick_ms) < RETAIN_EMERGENCY_MIN_INTERVAL_MS) {
        return;
    }

    if (s_flash_op_depth != 0U) {
        /* The main loop is inside a Flash operation: do not touch Flash or
         * the write position. retain_service() writes once it is free. */
        s_emergency_pending = true;
        return;
    }

    if (!retain_values_changed()) {
        return;
    }

    s_flash_op_depth++;
    bool ok = retain_write_record(true);
    s_flash_op_depth--;

    s_emergency_seen         = true;
    s_last_emergency_tick_ms = now;

    if (!ok) {
        /* Sector full (needs an erase) or the write did not verify: let
         * the main loop retry, where an erase is allowed. */
        s_emergency_pending = true;
    }
}

void retain_service(void)
{
    uint32_t now = sx_get_tick_ms();

    if (s_emergency_pending && s_flash_op_depth == 0U) {
        s_emergency_pending = false;
        if (retain_values_changed()) {
            (void)retain_snapshot_write();
            s_last_snapshot_tick_ms = now;
        }
    }

    /* Unsigned subtraction handles tick wraparound correctly as long as
     * the actual elapsed time never exceeds UINT32_MAX ms (~49.7 days) --
     * true for any period this project uses. Safety net only: the PVD
     * interrupt is the primary power-loss path, so a period with no change
     * costs no Flash write. */
    if ((now - s_last_snapshot_tick_ms) >= RETAIN_SNAPSHOT_PERIOD_MS) {
        if (retain_values_changed()) {
            (void)retain_snapshot_write();
        }
        s_last_snapshot_tick_ms = now;
    }
}