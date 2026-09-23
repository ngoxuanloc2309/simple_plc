/*
 * plc_tag.c - Layer 2 (PLC Core)
 *
 * Implementation of the Tag Table. This file must not include anything
 * from Layer 0/1; it only depends on standard C headers so it can be
 * compiled and unit tested on a plain PC toolchain.
 */

#include "plc_tag.h"

SPLC_Tag    g_tag_table[MAX_TAGS];
int32_t     g_tag_value[MAX_TAGS];

/*
 * Base index of each tag group, computed once by tag_table_load_from_flash()
 * from the SPLC_TagLayout it was given. DI's base is always 0 by
 * construction (see plc_tag.h's tag_di_base_index() doc-comment), so it is
 * not stored separately here.
 */
static uint16_t s_do_base;
static uint16_t s_ai_base;
static uint16_t s_vflag_base;
static uint16_t s_vreg_base;
static uint16_t s_vreg_retain_base;
static uint16_t s_counter_base;

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

void tag_table_load_from_flash(const SPLC_TagLayout *layout)
{
    /*
     * TODO: read the tag table from Flash once Layer 3 exposes a
     * platform-agnostic config loader. This function must not call
     * sx_flash_read() directly (that would pull a Layer 0/1 dependency
     * into Layer 2). Expected real implementation: receive a raw buffer
     * already read from Flash by Layer 3/4, validate it, and populate
     * g_tag_table[] from it.
     *
     * UNTIL THEN the table is filled purely from `layout`, in the fixed
     * wire group order (DI, DO, AI, VFLAG, VREG, VREG_RETAIN, COUNTER --
     * spec section 5.1), starting at index 0, with each group's base index
     * computed as the running sum of every group before it. This is NOT
     * optional: the board layer registers its pins via
     * plc_io_register_di/do/ai(), which reject any tag whose kind in
     * g_tag_table[] is not TAG_DI/TAG_DO/TAG_AI (plc_io.c). With an
     * all-TAG_NONE table every registration failed silently,
     * input_scan()/output_scan() skipped every channel, and no rule could
     * ever read a DI or drive a DO.
     */
    for (uint16_t i = 0; i < MAX_TAGS; i++) {
        g_tag_table[i].kind      = TAG_NONE;
        g_tag_table[i].channel   = 0;
        g_tag_table[i].reg_addr  = 0;
        g_tag_value[i]           = 0;
    }

    uint16_t di_base = 0U;

    s_do_base          = (uint16_t)(di_base           + layout->di_count);
    s_ai_base          = (uint16_t)(s_do_base         + layout->do_count);
    s_vflag_base       = (uint16_t)(s_ai_base         + layout->ai_count);
    s_vreg_base        = (uint16_t)(s_vflag_base      + layout->vflag_count);
    s_vreg_retain_base = (uint16_t)(s_vreg_base       + layout->vreg_count);
    s_counter_base     = (uint16_t)(s_vreg_retain_base + layout->vreg_retain_count);

    tag_fill_range(di_base,            layout->di_count,          TAG_DI);
    tag_fill_range(s_do_base,          layout->do_count,          TAG_DO);
    tag_fill_range(s_ai_base,          layout->ai_count,          TAG_AI);
    tag_fill_range(s_vflag_base,       layout->vflag_count,       TAG_VFLAG);
    tag_fill_range(s_vreg_base,        layout->vreg_count,        TAG_VREG);
    tag_fill_range(s_vreg_retain_base, layout->vreg_retain_count, TAG_VREG_RETAIN);
    tag_fill_range(s_counter_base,     layout->counter_count,     TAG_COUNTER);
    /* Any index past the last filled group stays TAG_NONE -- e.g. reserved
     * for a future Gateway SKU's TAG_MB_COIL/TAG_MB_HOLDING range (spec
     * 5.1), or simply unused headroom on a board with a smaller layout. */
}

uint16_t tag_di_base_index(void)          { return 0U; }
uint16_t tag_do_base_index(void)          { return s_do_base; }
uint16_t tag_ai_base_index(void)          { return s_ai_base; }
uint16_t tag_vflag_base_index(void)       { return s_vflag_base; }
uint16_t tag_vreg_base_index(void)        { return s_vreg_base; }
uint16_t tag_vreg_retain_base_index(void) { return s_vreg_retain_base; }
uint16_t tag_counter_base_index(void)     { return s_counter_base; }

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