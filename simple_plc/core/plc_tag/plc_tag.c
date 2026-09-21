/*
 * plc_tag.c - Layer 2 (PLC Core)
 *
 * Implementation of the Tag Table. This file must not include anything
 * from Layer 0/1; it only depends on standard C headers so it can be
 * compiled and unit tested on a plain PC toolchain.
 */

#include "plc_tag.h"
#include "plc_tag_def.h"

SPLC_Tag    g_tag_table[MAX_TAGS];
int32_t     g_tag_value[MAX_TAGS];

/*
 * Fill `count` consecutive slots starting at `first` with `kind`;
 * `channel` is the slot's index inside its own group (DI0 -> 0, DI1 -> 1,
 * ... DO0 -> 0, ...), i.e. the "local physical channel" SPLC_Tag.channel
 * documents. Slots outside 0..MAX_TAGS-1 are ignored, so a bad range can
 * never write past g_tag_table[].
 */
static void tag_fill_range(uint16_t first, uint16_t count, SPLC_TagKind kind)
{
    for (uint16_t i = 0; i < count; i++) {
        uint16_t idx = (uint16_t)(first + i);
        if (idx >= MAX_TAGS) {
            return;
        }
        g_tag_table[idx].kind     = (uint8_t)kind;
        g_tag_table[idx].channel  = (uint8_t)i;
        g_tag_table[idx].reg_addr = 0;
    }
}

void tag_table_load_from_flash(void)
{
    /*
     * TODO: read the tag table from Flash once Layer 3 exposes a
     * platform-agnostic config loader. This function must not call
     * sx_flash_read() directly (that would pull a Layer 0/1 dependency
     * into Layer 2). Expected real implementation: receive a raw buffer
     * already read from Flash by Layer 3/4, validate it, and populate
     * g_tag_table[] from it.
     *
     * UNTIL THEN the table is filled with the DEFAULT v1.9 layout defined
     * in plc_tag_def.h (spec section 5.1). This is NOT optional: the board
     * layer registers its pins via plc_io_register_di/do/ai(), which reject
     * any tag whose kind in g_tag_table[] is not TAG_DI/TAG_DO/TAG_AI
     * (plc_io.c). With an all-TAG_NONE table every registration failed
     * silently, input_scan()/output_scan() skipped every channel, and no
     * rule could ever read a DI or drive a DO.
     */
    for (uint16_t i = 0; i < MAX_TAGS; i++) {
        g_tag_table[i].kind      = TAG_NONE;
        g_tag_table[i].channel   = 0;
        g_tag_table[i].reg_addr  = 0;
        g_tag_value[i]           = 0;
    }

    tag_fill_range(TAG_DI0,       8U,  TAG_DI);
    tag_fill_range(TAG_DO0,       8U,  TAG_DO);
    tag_fill_range(TAG_AI0,       4U,  TAG_AI);
    tag_fill_range(TAG_VFLAG0,    32U, TAG_VFLAG);
    tag_fill_range(TAG_VREG0,     32U, TAG_VREG);
    tag_fill_range(TAG_VREG_R0,   32U, TAG_VREG_RETAIN);
    tag_fill_range(TAG_COUNTER0,  8U,  TAG_COUNTER);
    /* 124-127 stay TAG_NONE: reserved for a future Gateway SKU (spec 5.1). */
}

int32_t tag_read(uint16_t idx)
{
    if (idx >= MAX_TAGS) {
        return 0;
    }
    return g_tag_value[idx];
}

void tag_write(uint16_t idx, int32_t value)
{
    if (idx >= MAX_TAGS) {
        return;
    }
    g_tag_value[idx] = value;
}

SPLC_TagKind tag_get_kind(uint16_t idx)
{
    if (idx >= MAX_TAGS) {
        return TAG_NONE;
    }
    return (SPLC_TagKind)g_tag_table[idx].kind;
}