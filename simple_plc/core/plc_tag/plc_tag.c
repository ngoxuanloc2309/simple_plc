/*
 * plc_tag.c - Layer 2 (PLC Core)
 *
 * Implementation of the Tag Table. This file must not include anything
 * from Layer 0/1; it only depends on standard C headers so it can be
 * compiled and unit tested on a plain PC toolchain.
 */

#include "plc_tag.h"

Tag     g_tag_table[MAX_TAGS];
int32_t g_tag_value[MAX_TAGS];

void tag_table_load_from_flash(void)
{
    /*
     * TODO: replace this stub once Layer 3 exposes a platform-agnostic
     * config loader. This function must not call sx_flash_read() directly,
     * since that would pull a Layer 0/1 dependency into Layer 2.
     *
     * Expected real implementation: receive a raw buffer already read from
     * Flash by Layer 3/4, validate it, and populate g_tag_table[] from it.
     * For now this only guarantees a defined, all-zero starting state.
     */
    for (uint16_t i = 0; i < MAX_TAGS; i++) {
        g_tag_table[i].kind      = TAG_NONE;
        g_tag_table[i].channel   = 0;
        g_tag_table[i].reg_addr  = 0;
        g_tag_value[i]           = 0;
    }
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

TagKind tag_get_kind(uint16_t idx)
{
    if (idx >= MAX_TAGS) {
        return TAG_NONE;
    }
    return (TagKind)g_tag_table[idx].kind;
}