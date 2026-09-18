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

void plc_engine_scan_once(void)
{
    uint32_t t0 = sx_get_tick_ms();

    input_scan();
    rule_scan();
    output_scan();
    modbus_config_service();
    retain_service();

    uint32_t scan_time_ms = sx_get_tick_ms() - t0;
    plc_modbus_cfg_record_scan_time(scan_time_ms);
}