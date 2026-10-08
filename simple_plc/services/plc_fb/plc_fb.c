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

static timer_cfg_t   s_timer[PLC_FB_BLOCK_COUNT];
static counter_cfg_t s_counter[PLC_FB_BLOCK_COUNT];

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

void plc_fb_init(void)
{
    memset(s_timer, 0, sizeof(s_timer));
    for (uint16_t i = 0U; i < PLC_FB_BLOCK_COUNT; i++) {
        s_counter[i].mode       = PLC_FB_MODE_NONE;
        s_counter[i].preset     = 0;
        s_counter[i].retain_idx = PLC_FB_RETAIN_NONE;
    }
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

static bool config_valid(const timer_cfg_t *t, const counter_cfg_t *c)
{
    for (uint16_t i = 0U; i < PLC_FB_BLOCK_COUNT; i++) {
        if (t[i].mode > PLC_FB_TIMER_TP) {
            return false;
        }
    }

    for (uint16_t i = 0U; i < PLC_FB_BLOCK_COUNT; i++) {
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
                if (c[j].retain_idx == c[i].retain_idx) {
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

    /* Work on copies so a bad value anywhere leaves everything untouched. */
    timer_cfg_t   t[PLC_FB_BLOCK_COUNT];
    counter_cfg_t c[PLC_FB_BLOCK_COUNT];
    memcpy(t, s_timer, sizeof(t));
    memcpy(c, s_counter, sizeof(c));

    for (uint16_t k = 0U; k < quantity; k++) {
        uint16_t r   = (uint16_t)(offset + k);
        uint16_t v   = registers[k];
        uint16_t blk = (uint16_t)(r / PLC_FB_BLOCK_REGS);
        uint16_t f   = (uint16_t)(r % PLC_FB_BLOCK_REGS);

        if (blk < PLC_FB_BLOCK_COUNT) {
            timer_cfg_t *tc = &t[blk];
            switch (f) {
                case FB_F_MODE:      tc->mode = v; break;
                case FB_F_PRESET_HI: tc->pt_ms = (tc->pt_ms & 0x0000FFFFUL) | ((uint32_t)v << 16); break;
                case FB_F_PRESET_LO: tc->pt_ms = (tc->pt_ms & 0xFFFF0000UL) | v; break;
                default:             break;   /* firmware-owned: ignore */
            }
        } else {
            counter_cfg_t *cc = &c[blk - PLC_FB_BLOCK_COUNT];
            switch (f) {
                case FB_F_MODE:      cc->mode = v; break;
                case FB_F_PRESET_HI: cc->preset = (int32_t)(((uint32_t)cc->preset & 0x0000FFFFUL) | ((uint32_t)v << 16)); break;
                case FB_F_PRESET_LO: cc->preset = (int32_t)(((uint32_t)cc->preset & 0xFFFF0000UL) | v); break;
                case FB_F_RETAIN:    cc->retain_idx = v; break;
                default:             break;   /* firmware-owned: ignore */
            }
        }
    }

    if (!config_valid(t, c)) {
        return PLC_FB_WRITE_BAD_VALUE;
    }

    memcpy(s_timer, t, sizeof(t));
    memcpy(s_counter, c, sizeof(c));
    return PLC_FB_WRITE_OK;
}