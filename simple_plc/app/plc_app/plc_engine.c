#include "plc_engine.h"

#include "plc_tag.h"
#include "plc_rule.h"
#include "plc_rule_flash.h"
#include "plc_io.h"
#include "plc_retain.h"
#include "plc_modbus_cfg.h"
#include "plc_system_cmd_service.h"
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
    /*
     * board_get_tag_layout() is a plain data query (no GPIO/UART/USB touched
     * -- see its doc-comment in board.h), so it is safe to call before
     * board_init() brings up any real hardware. Its result must reach
     * tag_table_load_from_flash() BEFORE board_init() runs: board_init()
     * (via board_hw_init()) calls plc_io_register_di/do/ai(), which
     * validates each registration against g_tag_table[]'s kind -- that
     * table only has the right TAG_DI/TAG_DO/TAG_AI kinds once
     * tag_table_load_from_flash() has already populated it from this same
     * layout.
     */
    SPLC_TagLayout tag_layout = board_get_tag_layout();
    tag_table_load_from_flash(&tag_layout);

    rule_table_load_from_flash();
    plc_rule_flash_load();
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

    /*
     * Last thing in the scan cycle, on purpose: if this ends up calling
     * sx_system_reset() (a pending SPLC_SYSTEM_CMD_REBOOT whose grace
     * period has elapsed -- see plc_system_cmd_service.c), everything
     * else this cycle (retain_service()'s Flash write, the scan-time
     * measurement above, ...) has already run to completion first.
     */
    plc_system_cmd_service();
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