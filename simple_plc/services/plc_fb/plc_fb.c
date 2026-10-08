#include "plc_fb.h"

#include <stdbool.h>
#include <string.h>

#include "plc_tag.h"

/*
 * plc_fb.c - Layer 3. See plc_fb.h for the register contract and what this
 * module does (and does not) do yet.
 *
 * Register offsets inside one 8-register record. Both record kinds share
 * the first four; the rest differ (Structs doc sections 3.6 / 3.7):
 *   +0        status_bits            (firmware-owned)
 *   +1        mode                   (Host-written)
 *   +2..+3    pt_ms   / preset_value (Host-written, High Word first)
 *   +4..+5    et_ms   / current_value(firmware-owned, High Word first)
 *   +6        Timer: reserved   Counter: retain_tag_index (Host-written)
 *   +7        reserved
 */
#define FB_F_STATUS   0U
#define FB_F_MODE     1U
#define FB_F_PRESET_HI 2U
#define FB_F_PRESET_LO 3U
#define FB_F_VALUE_HI 4U
#define FB_F_VALUE_LO 5U
#define FB_F_RETAIN   6U   /* Counter only */

typedef struct {
    uint16_t mode;
    uint32_t pt_ms;
} timer_cfg_t;

typedef struct {
    uint16_t mode;
    int32_t  preset;
    uint16_t retain_idx;
} counter_cfg_t;

/* Running config: what FC03 reads return and what is saved to Flash. */
static timer_cfg_t   s_timer[PLC_FB_BLOCK_COUNT];
static counter_cfg_t s_counter[PLC_FB_BLOCK_COUNT];

/* Draft: what the Host has written since the last COMMIT / discard. A
 * block's draft content is meaningful only while its bit is set in
 * s_draft_mask (bit i = Timer i, bit 8 + i = Counter i). */
static timer_cfg_t   s_timer_draft[PLC_FB_BLOCK_COUNT];
static counter_cfg_t s_counter_draft[PLC_FB_BLOCK_COUNT];
static uint16_t      s_draft_mask;

#define MASK_TIMER(i)    ((uint16_t)(1U << (i)))
#define MASK_COUNTER(i)  ((uint16_t)(1U << (PLC_FB_BLOCK_COUNT + (i))))
#define MASK_ALL         ((uint16_t)0xFFFFU)

/* Counter i is the tag COUNTER[i]. It "exists" when the board's tag layout
 * actually has that many counters. */
static bool counter_exists(uint16_t i)
{
    uint32_t idx = (uint32_t)tag_counter_base_index() + i;
    return (idx < MAX_TAGS) && (tag_get_kind((uint16_t)idx) == TAG_COUNTER);
}

static bool counter_q(const counter_cfg_t *c, int32_t cv)
{
    switch (c->mode) {
        case PLC_FB_COUNTER_CTU: return cv >= c->preset;
        case PLC_FB_COUNTER_CTD: return cv <= 0;
        default:                 return false;
    }
}

static void counters_set_disabled(counter_cfg_t *c)
{
    for (uint16_t i = 0U; i < PLC_FB_BLOCK_COUNT; i++) {
        c[i].mode       = PLC_FB_MODE_NONE;
        c[i].preset     = 0;
        c[i].retain_idx = PLC_FB_RETAIN_NONE;
    }
}

static void running_set_disabled(void)
{
    memset(s_timer, 0, sizeof(s_timer));
    counters_set_disabled(s_counter);
}

void plc_fb_init(void)
{
    running_set_disabled();
    memset(s_timer_draft, 0, sizeof(s_timer_draft));
    counters_set_disabled(s_counter_draft);
    s_draft_mask = 0U;
}

/* Value of one register of the block, `r` = 0..PLC_FB_REG_COUNT-1. */
static uint16_t read_reg(uint16_t r)
{
    uint16_t blk = (uint16_t)(r / PLC_FB_BLOCK_REGS);
    uint16_t f   = (uint16_t)(r % PLC_FB_BLOCK_REGS);

    if (blk < PLC_FB_BLOCK_COUNT) {
        const timer_cfg_t *t = &s_timer[blk];
        switch (f) {
            case FB_F_MODE:      return t->mode;
            case FB_F_PRESET_HI: return (uint16_t)(t->pt_ms >> 16);
            case FB_F_PRESET_LO: return (uint16_t)(t->pt_ms & 0xFFFFU);
            default:             return 0U;   /* status, ET, reserved: not tracked */
        }
    }

    uint16_t i = (uint16_t)(blk - PLC_FB_BLOCK_COUNT);
    const counter_cfg_t *c = &s_counter[i];
    int32_t cv = 0;
    if (c->mode != PLC_FB_MODE_NONE && counter_exists(i)) {
        cv = tag_read((uint16_t)(tag_counter_base_index() + i));
    }

    switch (f) {
        case FB_F_STATUS:    return counter_q(c, cv) ? PLC_FB_CNT_STATUS_Q : 0U;
        case FB_F_MODE:      return c->mode;
        case FB_F_PRESET_HI: return (uint16_t)((uint32_t)c->preset >> 16);
        case FB_F_PRESET_LO: return (uint16_t)((uint32_t)c->preset & 0xFFFFU);
        case FB_F_VALUE_HI:  return (uint16_t)((uint32_t)cv >> 16);
        case FB_F_VALUE_LO:  return (uint16_t)((uint32_t)cv & 0xFFFFU);
        case FB_F_RETAIN:    return c->retain_idx;
        default:             return 0U;
    }
}

void plc_fb_read(uint16_t offset, uint16_t quantity, uint16_t *registers_out)
{
    for (uint16_t k = 0U; k < quantity; k++) {
        uint16_t r = (uint16_t)(offset + k);
        registers_out[k] = (r < PLC_FB_REG_COUNT) ? read_reg(r) : 0U;
    }
}

/*
 * Validate the blocks selected by `mask` (see MASK_*). Blocks outside the
 * mask are not looked at -- in particular they take no part in the
 * duplicate-retain check.
 */
static bool config_valid(const timer_cfg_t *t, const counter_cfg_t *c, uint16_t mask)
{
    for (uint16_t i = 0U; i < PLC_FB_BLOCK_COUNT; i++) {
        if ((mask & MASK_TIMER(i)) != 0U && t[i].mode > PLC_FB_TIMER_TP) {
            return false;
        }
    }

    for (uint16_t i = 0U; i < PLC_FB_BLOCK_COUNT; i++) {
        if ((mask & MASK_COUNTER(i)) == 0U) {
            continue;
        }
        if (c[i].mode > PLC_FB_COUNTER_CTD) {
            return false;
        }
        if (c[i].mode != PLC_FB_MODE_NONE && !counter_exists(i)) {
            return false;
        }
        if (c[i].retain_idx != PLC_FB_RETAIN_NONE) {
            if (c[i].retain_idx >= MAX_TAGS ||
                tag_get_kind(c[i].retain_idx) != TAG_VREG_RETAIN) {
                return false;
            }
            for (uint16_t j = 0U; j < i; j++) {
                if ((mask & MASK_COUNTER(j)) != 0U && c[j].retain_idx == c[i].retain_idx) {
                    return false;   /* one retain slot cannot back two counters */
                }
            }
        }
    }
    return true;
}

plc_fb_write_result_t plc_fb_write(uint16_t offset, uint16_t quantity,
                                   const uint16_t *registers)
{
    if (offset >= PLC_FB_REG_COUNT || quantity > (uint16_t)(PLC_FB_REG_COUNT - offset)) {
        return PLC_FB_WRITE_BAD_ADDRESS;
    }

    /* Work on copies of the draft so a bad value anywhere leaves it untouched. */
    timer_cfg_t   t[PLC_FB_BLOCK_COUNT];
    counter_cfg_t c[PLC_FB_BLOCK_COUNT];
    uint16_t      mask = s_draft_mask;
    memcpy(t, s_timer_draft, sizeof(t));
    memcpy(c, s_counter_draft, sizeof(c));

    for (uint16_t k = 0U; k < quantity; k++) {
        uint16_t r   = (uint16_t)(offset + k);
        uint16_t v   = registers[k];
        uint16_t blk = (uint16_t)(r / PLC_FB_BLOCK_REGS);
        uint16_t f   = (uint16_t)(r % PLC_FB_BLOCK_REGS);

        if (blk < PLC_FB_BLOCK_COUNT) {
            if ((mask & MASK_TIMER(blk)) == 0U) {
                t[blk] = s_timer[blk];          /* first touch: start from running config */
                mask  |= MASK_TIMER(blk);
            }
            timer_cfg_t *tc = &t[blk];
            switch (f) {
                case FB_F_MODE:      tc->mode = v; break;
                case FB_F_PRESET_HI: tc->pt_ms = (tc->pt_ms & 0x0000FFFFUL) | ((uint32_t)v << 16); break;
                case FB_F_PRESET_LO: tc->pt_ms = (tc->pt_ms & 0xFFFF0000UL) | v; break;
                default:             break;   /* firmware-owned: ignore (block still counts as written) */
            }
        } else {
            uint16_t i = (uint16_t)(blk - PLC_FB_BLOCK_COUNT);
            if ((mask & MASK_COUNTER(i)) == 0U) {
                c[i]  = s_counter[i];
                mask |= MASK_COUNTER(i);
            }
            counter_cfg_t *cc = &c[i];
            switch (f) {
                case FB_F_MODE:      cc->mode = v; break;
                case FB_F_PRESET_HI: cc->preset = (int32_t)(((uint32_t)cc->preset & 0x0000FFFFUL) | ((uint32_t)v << 16)); break;
                case FB_F_PRESET_LO: cc->preset = (int32_t)(((uint32_t)cc->preset & 0xFFFF0000UL) | v); break;
                case FB_F_RETAIN:    cc->retain_idx = v; break;
                default:             break;   /* firmware-owned: ignore (block still counts as written) */
            }
        }
    }

    if (!config_valid(t, c, mask)) {
        return PLC_FB_WRITE_BAD_VALUE;
    }

    memcpy(s_timer_draft, t, sizeof(t));
    memcpy(s_counter_draft, c, sizeof(c));
    s_draft_mask = mask;
    return PLC_FB_WRITE_OK;
}

void plc_fb_discard_draft(void)
{
    memset(s_timer_draft, 0, sizeof(s_timer_draft));
    counters_set_disabled(s_counter_draft);
    s_draft_mask = 0U;
}

void plc_fb_commit_draft(void)
{
    for (uint16_t i = 0U; i < PLC_FB_BLOCK_COUNT; i++) {
        if ((s_draft_mask & MASK_TIMER(i)) != 0U) {
            s_timer[i] = s_timer_draft[i];
        } else {
            memset(&s_timer[i], 0, sizeof(s_timer[i]));
        }
    }
    for (uint16_t i = 0U; i < PLC_FB_BLOCK_COUNT; i++) {
        if ((s_draft_mask & MASK_COUNTER(i)) != 0U) {
            s_counter[i] = s_counter_draft[i];
        } else {
            s_counter[i].mode       = PLC_FB_MODE_NONE;
            s_counter[i].preset     = 0;
            s_counter[i].retain_idx = PLC_FB_RETAIN_NONE;
        }
    }
    plc_fb_discard_draft();
}

void plc_fb_clear_all(void)
{
    running_set_disabled();
    plc_fb_discard_draft();
}

/* ---- Flash image (big-endian) ------------------------------------------ */

static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static uint16_t get_u16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
static uint32_t get_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

#define IMG_TIMER_SIZE    6U
#define IMG_COUNTER_SIZE  8U

_Static_assert(PLC_FB_BLOCK_COUNT * (IMG_TIMER_SIZE + IMG_COUNTER_SIZE) == PLC_FB_FLASH_SIZE,
               "PLC_FB_FLASH_SIZE out of sync with the image layout");

void plc_fb_export(uint8_t out[PLC_FB_FLASH_SIZE])
{
    uint8_t *p = out;
    for (uint16_t i = 0U; i < PLC_FB_BLOCK_COUNT; i++) {
        put_u16(p, s_timer[i].mode);
        put_u32(p + 2U, s_timer[i].pt_ms);
        p += IMG_TIMER_SIZE;
    }
    for (uint16_t i = 0U; i < PLC_FB_BLOCK_COUNT; i++) {
        put_u16(p, s_counter[i].mode);
        put_u32(p + 2U, (uint32_t)s_counter[i].preset);
        put_u16(p + 6U, s_counter[i].retain_idx);
        p += IMG_COUNTER_SIZE;
    }
}

bool plc_fb_import(const uint8_t in[PLC_FB_FLASH_SIZE])
{
    timer_cfg_t   t[PLC_FB_BLOCK_COUNT];
    counter_cfg_t c[PLC_FB_BLOCK_COUNT];
    const uint8_t *p = in;

    for (uint16_t i = 0U; i < PLC_FB_BLOCK_COUNT; i++) {
        t[i].mode  = get_u16(p);
        t[i].pt_ms = get_u32(p + 2U);
        p += IMG_TIMER_SIZE;
    }
    for (uint16_t i = 0U; i < PLC_FB_BLOCK_COUNT; i++) {
        c[i].mode       = get_u16(p);
        c[i].preset     = (int32_t)get_u32(p + 2U);
        c[i].retain_idx = get_u16(p + 6U);
        p += IMG_COUNTER_SIZE;
    }

    plc_fb_discard_draft();
    if (!config_valid(t, c, MASK_ALL)) {
        return false;
    }
    memcpy(s_timer, t, sizeof(t));
    memcpy(s_counter, c, sizeof(c));
    return true;
}