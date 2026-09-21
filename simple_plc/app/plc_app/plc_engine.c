#include "plc_engine.h"

#include "plc_tag.h"
#include "plc_rule.h"
#include "plc_io.h"
#include "plc_retain.h"
#include "plc_modbus_cfg.h"
#include "board.h"
#include "sx_time.h"

/*
 * plc_engine.c - Layer 4 (Engine & Application entry)
 *
 * See plc_engine.h for the full ordering rationale on both functions
 * below. Nothing in this file is SKU-specific -- every per-board detail
 * (pin wiring, which transport backs the config channel, DI/DO/AI
 * counts) lives behind board_init()/board_get_modbus_transport() and
 * SPLC_DeviceResourceInfo, both set up by board_*.c before this file
 * ever runs.
 */

void plc_engine_init(void)
{
    tag_table_load_from_flash();
    rule_table_load_from_flash();
    retain_store_restore();

    board_init();

    modbus_transport_t transport = board_get_modbus_transport();
    plc_modbus_cfg_init(&transport);
}

/*
 * The one place a scan cycle is defined. `now_ms` is the tick sampled at
 * cycle start: it is what rule_scan() sees and the reference point for the
 * scan-duration measurement.
 */
static void scan_cycle(uint32_t now_ms)
{
    input_scan();
    rule_scan(now_ms);
    output_scan();
    modbus_config_service();
    retain_service();

    plc_modbus_cfg_record_scan_time(sx_get_tick_ms() - now_ms);
}

void plc_engine_scan_once(void)
{
    scan_cycle(sx_get_tick_ms());
}

void plc_engine_poll(void)
{
    static uint32_t s_last_scan_tick_ms = 0U;

    uint32_t now = sx_get_tick_ms();
    if ((uint32_t)(now - s_last_scan_tick_ms) >= PLC_SCAN_INTERVAL_MS) {
        s_last_scan_tick_ms = now;
        scan_cycle(now);
    }
}