#ifndef PLC_IO_H
#define PLC_IO_H

/*
 * plc_io.h - Layer 3 (PLC Application Services)
 *
 * Bridges the hardware-agnostic Tag Table (Layer 2, core/plc_tag) to real
 * pins/channels via the Layer 1 driver contracts (components/gpio,
 * components/adc). This file must not include anything from Layer 0
 * (platform headers) -- only Layer 1 contracts (sx_gpio.h, sx_adc.h) and
 * Layer 2 (plc_tag.h).
 *
 * Design: registration API, not hardcoded parallel arrays.
 * docs/architecture.md section 10 flags the parallel-array approach
 * (g_di_tag_map[]/g_di_gpio_pin[]) as risky -- two arrays indexed in
 * lockstep can silently drift out of sync as channels are added/removed.
 * Instead, Layer 4 (app/board init) calls plc_io_register_di/do/ai() once
 * per channel at boot; this file owns the single source of truth mapping
 * tag_index -> hardware descriptor internally.
 *
 * Registration validates the tag's declared kind (via tag_get_kind())
 * matches the register call, catching a wrong tag_index/wrong register
 * call mistake at init time instead of silently misbehaving at runtime.
 *
 * No dynamic heap: MAX_IO_CHANNELS below is a fixed compile-time bound,
 * not tied to any specific board's real DI/DO/AI count -- boards with
 * fewer channels than declared here simply register fewer of them.
 *
 * See docs/architecture.md, section 3.1 and section 10.
 */

#include <stdbool.h>
#include <stdint.h>

#include "sx_gpio.h"
#include "sx_adc.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Upper bound on how many DI/DO/AI channels this service can track at
 * once. Deliberately sized to the widest case any SKU built on the
 * v1.9 wire layout could need (8 DI + 8 DO + 4 AI, per
 * docs/SimplePLC_App_MCU_Structs_v1.9_Self_Describing_Profile.md section
 * 5.1) rather than to any one board's actual channel count -- a board
 * with fewer physical channels (e.g. this repo's own
 * RS485_IO_RF_V2.ioc, which currently exposes 4 DI / 4 DO / 1 AI) just
 * registers fewer of them. Layer 3 does not need or want to know the
 * real count at compile time.
 */
#define PLC_IO_MAX_DI 8
#define PLC_IO_MAX_DO 8
#define PLC_IO_MAX_AI 4

/*
 * Register a digital input channel. tag_idx must already be loaded into
 * g_tag_table[] as TAG_DI (see core/plc_tag/plc_tag.h,
 * tag_table_load_from_flash()) -- this function reads that kind back via
 * tag_get_kind() and refuses the registration if it does not match,
 * rather than silently accepting a mismatched tag_index.
 *
 * pin: caller-owned sx_gpio_pin_t already configured for
 *      SX_GPIO_MODE_INPUT[_PULLUP|_PULLDOWN] via sx_gpio_init() -- this
 *      function does not call sx_gpio_init() itself, since pull
 *      configuration is a per-board wiring decision (Layer 4), not a
 *      Layer 3 concern.
 *
 * returns: true on success, false if tag_idx's kind is not TAG_DI, the
 *          DI registration table is full, or pin is NULL.
 */
bool plc_io_register_di(uint16_t tag_idx, sx_gpio_pin_t *pin);

/*
 * Register a digital output channel. Same tag-kind validation as
 * plc_io_register_di(), checked against TAG_DO.
 *
 * pin: caller-owned sx_gpio_pin_t already configured for
 *      SX_GPIO_MODE_OUTPUT_PP via sx_gpio_init() (with whatever initial
 *      value Layer 4 wants at boot -- this function does not re-init the
 *      pin or assume an initial value).
 *
 * returns: true on success, false if tag_idx's kind is not TAG_DO, the
 *          DO registration table is full, or pin is NULL.
 */
bool plc_io_register_do(uint16_t tag_idx, sx_gpio_pin_t *pin);

/*
 * Register an analog input channel. Same tag-kind validation as
 * plc_io_register_di(), checked against TAG_AI.
 *
 * config: caller-owned sx_adc_config_t already configured via
 *         sx_adc_init() -- this function does not call sx_adc_init()
 *         itself, since ADC resolution is a per-board decision (Layer 4).
 *
 * returns: true on success, false if tag_idx's kind is not TAG_AI, the
 *          AI registration table is full, or config is NULL.
 */
bool plc_io_register_ai(uint16_t tag_idx, sx_adc_config_t *config);

/*
 * Reset all DI/DO/AI registrations to empty. Intended for test setup
 * (re-registering a fresh set of channels between test cases) and for
 * SPLC_SYSTEM_CMD_FACTORY_RESET handling once that lands in Layer 4 --
 * does not touch g_tag_table[]/g_tag_value[] (Layer 2 owns those).
 */
void plc_io_reset(void);

/*
 * Poll every registered DI/AI channel once and write the value into the
 * Tag Table via tag_write() (see core/plc_tag/plc_tag.h -- input_scan()
 * is the only code in the system allowed to write TAG_DI/TAG_AI values,
 * per the pull-model rule in docs/architecture.md section 6).
 *
 * DI: sx_gpio_read() returns SX_GPIO_HIGH/SX_GPIO_LOW; stored as the
 *     tag's int32_t value using 1/0 (not the raw enum), so rule_scan()'s
 *     edge/compare logic (which works in plain integers) never needs to
 *     know about sx_gpio_value_t.
 * AI: sx_adc_read() returns the raw ADC code (0..4095 at 12-bit
 *     resolution, or less at a narrower resolution set via sx_adc_init()
 *     -- resolution is a Layer 4/board decision, not converted or scaled
 *     here). Scaling/calibration (AIScaleConfig) is explicitly out of
 *     scope per docs/SimplePLC_RuleStruct_MCU_Spec_v0.1.md section 8.1 --
 *     a rule using SPLC_ACT_SCALE_TAG can do that conversion in-engine
 *     if needed.
 *
 * Called once per scan cycle (every 10 ms), before rule_scan() -- see
 * docs/architecture.md section 4.2's plc_engine_scan_once() ordering.
 */
void input_scan(void);

/*
 * Read every registered DO channel's current value via tag_read() and
 * drive the corresponding physical pin via sx_gpio_write(). A tag value
 * of 0 drives SX_GPIO_LOW; any nonzero value drives SX_GPIO_HIGH (mirrors
 * the boolean convention rule_scan()'s SPLC_ACT_TOGGLE_TAG already uses
 * for tag values -- see core/plc_internal_rule/plc_rule_action.c).
 *
 * Called once per scan cycle, after rule_scan() -- see
 * docs/architecture.md section 4.2.
 */
void output_scan(void);

#ifdef __cplusplus
}
#endif

#endif /* PLC_IO_H */