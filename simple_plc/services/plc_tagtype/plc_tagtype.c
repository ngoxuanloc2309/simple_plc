/*
 * plc_tagtype.c - Layer 3. See plc_tagtype.h.
 */
#include "plc_tagtype.h"

#if SPLC_REAL_ENABLE

#include <string.h>
#include "plc_tag.h"
#include "splc_log.h"

static const char *TAG = "PLC_TAGTYPE";

static uint8_t s_types[PLC_TAGTYPE_COUNT];   /* RUNNING table, one byte per tag */
static uint8_t s_draft[PLC_TAGTYPE_COUNT];   /* DRAFT, valid only while s_draft_active */
static bool    s_draft_active;

/* Kinds that may be typed REAL. */
static bool kind_allows_real(uint8_t kind)
{
    return kind == TAG_AI || kind == TAG_VREG || kind == TAG_VREG_RETAIN || kind == TAG_MB_HOLDING;
}

/* Kinds whose stored value is project state that must not be reinterpreted
 * when the tag switches between an integer type and REAL. */
static bool kind_holds_state(uint8_t kind)
{
    return kind == TAG_VREG || kind == TAG_VREG_RETAIN || kind == TAG_MB_HOLDING;
}

static bool image_valid(const uint8_t *img)
{
    for (uint16_t i = 0U; i < PLC_TAGTYPE_COUNT; i++) {
        const uint8_t t = img[i];
        if (t > (uint8_t)PLC_TAGTYPE_REAL) {
            return false;
        }
        if (t == (uint8_t)PLC_TAGTYPE_REAL && i < MAX_TAGS &&
            !kind_allows_real((uint8_t)tag_get_kind(i))) {
            return false;   /* REAL on a tag whose kind cannot hold one (tags >= MAX_TAGS do not exist: ignored) */
        }
    }
    return true;
}

void plc_tagtype_init(void)
{
    plc_tagtype_clear_all();
}

void plc_tagtype_clear_all(void)
{
    memset(s_types, 0, sizeof(s_types));
    memset(s_draft, 0, sizeof(s_draft));
    s_draft_active = false;
}

plc_tagtype_t plc_tagtype_get(uint16_t tag)
{
    if (tag >= PLC_TAGTYPE_COUNT || s_types[tag] == (uint8_t)PLC_TAGTYPE_UNDECLARED) {
        return PLC_TAGTYPE_INT32;
    }
    return (plc_tagtype_t)s_types[tag];
}

bool plc_tagtype_is_real(uint16_t tag)
{
    return plc_tagtype_get(tag) == PLC_TAGTYPE_REAL;
}

void plc_tagtype_read(uint16_t offset, uint16_t quantity, uint16_t *registers_out)
{
    for (uint16_t i = 0U; i < quantity; i++) {
        const uint16_t k = (uint16_t)(offset + i);
        registers_out[i] = (uint16_t)(((uint16_t)s_types[2U * k] << 8) | s_types[2U * k + 1U]);
    }
}

plc_tagtype_write_result_t plc_tagtype_write(uint16_t offset, uint16_t quantity,
                                             const uint16_t *registers)
{
    if (quantity == 0U || (uint32_t)offset + quantity > PLC_TAGTYPE_REGS) {
        return PLC_TAGTYPE_WRITE_BAD_ADDRESS;
    }
    if (!s_draft_active) {
        memcpy(s_draft, s_types, sizeof(s_draft));   /* a partial write keeps the rest */
        s_draft_active = true;
    }
    for (uint16_t i = 0U; i < quantity; i++) {
        const uint16_t k = (uint16_t)(offset + i);
        s_draft[2U * k]      = (uint8_t)(registers[i] >> 8);
        s_draft[2U * k + 1U] = (uint8_t)(registers[i] & 0xFFU);
    }
    return PLC_TAGTYPE_WRITE_OK;
}

bool plc_tagtype_validate_draft(void)
{
    return !s_draft_active || image_valid(s_draft);
}

void plc_tagtype_commit_draft(void)
{
    if (!s_draft_active) {
        return;
    }
    uint16_t reset = 0U;
    for (uint16_t i = 0U; i < MAX_TAGS && i < PLC_TAGTYPE_COUNT; i++) {
        const bool was_real = (s_types[i] == (uint8_t)PLC_TAGTYPE_REAL);
        const bool is_real  = (s_draft[i] == (uint8_t)PLC_TAGTYPE_REAL);
        if (was_real != is_real && kind_holds_state((uint8_t)tag_get_kind(i))) {
            tag_write(i, 0);   /* the old bits would be read as the other kind of number */
            reset++;
        }
    }
    memcpy(s_types, s_draft, sizeof(s_types));
    s_draft_active = false;
    if (reset != 0U) {
        log_info(TAG, "commit: %u tag(s) changed between integer and REAL, value reset to 0",
                 (unsigned)reset);
    }
}

void plc_tagtype_discard_draft(void)
{
    s_draft_active = false;
}

void plc_tagtype_export(uint8_t *out)
{
    memcpy(out, s_types, PLC_TAGTYPE_FLASH_SIZE);
}

bool plc_tagtype_import(const uint8_t *in)
{
    if (!image_valid(in)) {
        return false;
    }
    memcpy(s_types, in, PLC_TAGTYPE_FLASH_SIZE);
    s_draft_active = false;
    return true;
}

#else
typedef int plc_tagtype_translation_unit_is_empty_without_real;   /* keep ISO C happy */
#endif /* SPLC_REAL_ENABLE */