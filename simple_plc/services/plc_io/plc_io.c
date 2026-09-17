/*
 * plc_io.c - Layer 3 (PLC Application Services)
 *
 * Implementation of the registration API declared in plc_io.h. See that
 * file's header comment for the design rationale (registration API over
 * parallel arrays) and docs/architecture.md section 3.1 / section 10.
 */

#include "plc_io.h"

#include <stddef.h>

#include "plc_tag.h"

/* One registration slot: which tag index this hardware descriptor feeds
 * into/reads from. registered=false means the slot is empty. */
typedef struct {
    bool           registered;
    uint16_t       tag_idx;
    sx_gpio_pin_t *pin;
} plc_io_di_slot_t;

typedef struct {
    bool           registered;
    uint16_t       tag_idx;
    sx_gpio_pin_t *pin;
} plc_io_do_slot_t;

typedef struct {
    bool             registered;
    uint16_t         tag_idx;
    sx_adc_config_t *config;
} plc_io_ai_slot_t;

static plc_io_di_slot_t s_di_table[PLC_IO_MAX_DI];
static plc_io_do_slot_t s_do_table[PLC_IO_MAX_DO];
static plc_io_ai_slot_t s_ai_table[PLC_IO_MAX_AI];

static uint16_t s_di_count = 0;
static uint16_t s_do_count = 0;
static uint16_t s_ai_count = 0;

void plc_io_reset(void)
{
    for (int i = 0; i < PLC_IO_MAX_DI; i++) {
        s_di_table[i].registered = false;
        s_di_table[i].pin        = NULL;
    }
    for (int i = 0; i < PLC_IO_MAX_DO; i++) {
        s_do_table[i].registered = false;
        s_do_table[i].pin        = NULL;
    }
    for (int i = 0; i < PLC_IO_MAX_AI; i++) {
        s_ai_table[i].registered = false;
        s_ai_table[i].config     = NULL;
    }
    s_di_count = 0;
    s_do_count = 0;
    s_ai_count = 0;
}

bool plc_io_register_di(uint16_t tag_idx, sx_gpio_pin_t *pin)
{
    if (pin == NULL || tag_get_kind(tag_idx) != TAG_DI || s_di_count >= PLC_IO_MAX_DI) {
        return false;
    }

    s_di_table[s_di_count].registered = true;
    s_di_table[s_di_count].tag_idx    = tag_idx;
    s_di_table[s_di_count].pin        = pin;
    s_di_count++;
    return true;
}

bool plc_io_register_do(uint16_t tag_idx, sx_gpio_pin_t *pin)
{
    if (pin == NULL || tag_get_kind(tag_idx) != TAG_DO || s_do_count >= PLC_IO_MAX_DO) {
        return false;
    }

    s_do_table[s_do_count].registered = true;
    s_do_table[s_do_count].tag_idx    = tag_idx;
    s_do_table[s_do_count].pin        = pin;
    s_do_count++;
    return true;
}

bool plc_io_register_ai(uint16_t tag_idx, sx_adc_config_t *config)
{
    if (config == NULL || tag_get_kind(tag_idx) != TAG_AI || s_ai_count >= PLC_IO_MAX_AI) {
        return false;
    }

    s_ai_table[s_ai_count].registered = true;
    s_ai_table[s_ai_count].tag_idx    = tag_idx;
    s_ai_table[s_ai_count].config     = config;
    s_ai_count++;
    return true;
}

void input_scan(void)
{
    for (uint16_t i = 0; i < s_di_count; i++) {
        if (!s_di_table[i].registered) {
            continue;
        }
        sx_gpio_value_t v = sx_gpio_read(s_di_table[i].pin);
        tag_write(s_di_table[i].tag_idx, (v == SX_GPIO_HIGH) ? 1 : 0);
    }

    for (uint16_t i = 0; i < s_ai_count; i++) {
        if (!s_ai_table[i].registered) {
            continue;
        }
        uint32_t raw = sx_adc_read(s_ai_table[i].config);
        /* No scaling/calibration here -- see plc_io.h's input_scan()
         * comment; raw ADC code stored as-is. Cast is safe: sx_adc_read()
         * returns at most a 12-bit code (0..4095), well within int32_t
         * range. */
        tag_write(s_ai_table[i].tag_idx, (int32_t)raw);
    }
}

void output_scan(void)
{
    for (uint16_t i = 0; i < s_do_count; i++) {
        if (!s_do_table[i].registered) {
            continue;
        }
        int32_t v = tag_read(s_do_table[i].tag_idx);
        sx_gpio_write(s_do_table[i].pin, (v != 0) ? SX_GPIO_HIGH : SX_GPIO_LOW);
    }
}