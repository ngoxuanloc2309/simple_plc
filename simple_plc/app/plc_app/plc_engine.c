#include "plc_engine.h"

#include "plc_tag.h"
#include "plc_rule.h"
#include "plc_rule_flash.h"
#include "plc_io.h"
#include "plc_retain.h"
#include "plc_modbus_cfg.h"
#include "plc_rtc.h"
#include "plc_fb.h"
#include "plc_mqtt.h"
#include "plc_tagtype.h"
#include "plc_mbm.h"
#include "plc_mbw.h"
#include "plc_wifi.h"
#include "plc_mqtt_tls.h"
#if SPLC_MQTT_SUB_ENABLE
#include "plc_mqtt_sub.h"
#endif
#include "plc_system_cmd_service.h"
#include "splc_board.h"
#include "sx_time.h"
#include "sx_pwd.h"

/*
 * plc_engine.c - Layer 4 (Engine & Application entry)
 *
 * See plc_engine.h for the full ordering rationale on both functions
 * below. Nothing in this file is SKU-specific -- every per-board detail
 * (pin wiring, which transport backs the config channel, DI/DO/AI
 * counts) lives behind splc_board_init()/board_get_modbus_transport() and
 * SPLC_DeviceResourceInfo, both set up by board_*.c before this file
 * ever runs.
 */

/*
 * Tick sampled at the start of the previous scan cycle. Feeds the real
 * elapsed time (not the nominal PLC_SCAN_INTERVAL_MS) to
 * plc_modbus_cfg_diag_tick(), so the diagnostic lease measures wall-clock
 * time even when a cycle overruns. Seeded at the end of plc_engine_init()
 * so the first cycle does not see boot time as elapsed time.
 */
static uint32_t s_prev_cycle_tick_ms = 0U;

void plc_engine_init(void)
{
    /*
     * board_get_tag_layout() is a plain data query (no GPIO/UART/USB touched
     * -- see its doc-comment in splc_board.h), so it is safe to call before
     * splc_board_init() brings up any real hardware. Its result must reach
     * tag_table_load_from_flash() BEFORE splc_board_init() runs: splc_board_init()
     * (via board_hw_init()) calls plc_io_register_di/do/ai(), which
     * validates each registration against g_tag_table[]'s kind -- that
     * table only has the right TAG_DI/TAG_DO/TAG_AI kinds once
     * tag_table_load_from_flash() has already populated it from this same
     * layout.
     */
    SPLC_TagLayout tag_layout = board_get_tag_layout();
    tag_table_load_from_flash(&tag_layout);
    plc_fb_init();
#if SPLC_REAL_ENABLE
    plc_tagtype_init();   /* before plc_rule_flash_load(): the Flash record will fill it (decision 41) */
#endif
#if SPLC_MQTT_ENABLE
    plc_mqtt_init();   /* before plc_rule_flash_load(): the Flash record will fill it (step E.3) */
#endif
#if SPLC_MBM_ENABLE
    plc_mbm_init();    /* before plc_rule_flash_load(): the Flash record will fill it (decision 41) */
#endif
#if SPLC_MBW_ENABLE
    plc_mbw_init();    /* before plc_rule_flash_load(): the Flash record will fill it (decision 48) */
#endif
#if SPLC_MQTT_SUB_ENABLE
    plc_mqtt_sub_init();   /* before plc_rule_flash_load(): the Flash record will fill it (decision 47) */
#endif

    rule_table_load_from_flash();
    plc_rule_flash_load();
    retain_store_restore();

#if PLC_PVD_EMERGENCY_SAVE_ENABLE
    /*
     * Low-voltage (PVD) early warning -> emergency retain write. Registered
     * here, after retain_store_restore() has established the Flash write
     * position the interrupt will use, and as the last step that can fail
     * quietly: registering also arms the PVD interrupt in the NVIC, so
     * nothing runs from it before this point.
     */
    sx_power_register_low_voltage_callback(retain_emergency_snapshot);
#endif

    splc_board_init();

    modbus_transport_t transport = board_get_modbus_transport();
    plc_modbus_cfg_init(&transport);

    s_prev_cycle_tick_ms = sx_get_tick_ms();
}

/*
 * The one place a scan cycle is defined. `now_ms` is the tick sampled at
 * cycle start: it is what rule_scan() sees and the reference point for the
 * scan-duration measurement.
 */
static void scan_cycle(uint32_t now_ms)
{
    /*
     * First, on purpose: if the diagnostic lease runs out in this tick,
     * Tag Store ownership is back with the Rule Engine BEFORE the checks
     * below, so this same cycle already runs a full pass on fresh inputs
     * (Wire Contract section 1, invariant 4).
     */
    plc_modbus_cfg_diag_tick((uint32_t)(now_ms - s_prev_cycle_tick_ms));
    s_prev_cycle_tick_ms = now_ms;

    input_scan();

    /*
     * Suspended while a Host holds DIAG_CONTROL (Wire Contract section 1,
     * invariant 2). input_scan()/output_scan() still run: DO pins keep
     * following their tags and DI/AI tags stay current for the Host to
     * read.
     *
     * Decision (board_dev, agreed with the App team): when the Rule Engine
     * resumes -- CMD_EXIT_DIAG or lease expiry, both end up here because
     * diag_tick() ran above -- every rule's runtime state is reset, so the
     * rules run again exactly as after a fresh load. Rule table unchanged.
     * Consequence (same as a cold start): an input already HIGH at that
     * moment counts as a rising edge for ON_RISE rules on the first pass.
     */
    static bool s_rule_engine_was_suspended = false;
    bool suspended = plc_modbus_cfg_is_rule_engine_suspended();
    if (!suspended) {
        if (s_rule_engine_was_suspended) {
            rule_runtime_reset();
        }
#if SPLC_MQTT_SUB_ENABLE
        /* Remote commands land in the tags BEFORE the rules run, so this very scan's
         * rules already see them (plc_mqtt_sub.h). Not while a diag session forces
         * the tags: the Host owns them then. */
        plc_mqtt_sub_scan(now_ms);
#endif
        /*
         * Local time of day for TRG_TIME_WINDOW (Structs doc V2.0 section
         * 7), read once per cycle so every rule in the pass sees the same
         * time. RULE_HHMM_INVALID while the RTC is not synced or its
         * timezone has not been written yet: Time Window rules then do
         * not fire.
         */
        uint16_t hhmm = 0U;
        rule_scan(now_ms, plc_rtc_get_local_hhmm(&hhmm) ? (uint32_t)hhmm
                                                         : RULE_HHMM_INVALID);

        /*
         * Timer telemetry (ET / RUNNING / Q at 0x0B00..): read from the
         * rules' runtime state this very pass just updated, with the same
         * now_ms. Skipped while the engine is suspended, so the values hold
         * their last reading in DIAG_CONTROL.
         */
        plc_fb_scan(now_ms);
    }
    s_rule_engine_was_suspended = suspended;

    output_scan();
#if SPLC_MQTT_ENABLE
    /* Not while a diag session suspends the engine: the tags are forced by the
     * Host then, not real data (docs/plc_mqtt_design.md, Q4). */
    if (!suspended) {
        plc_mqtt_scan(now_ms);
    }
#endif
#if SPLC_MBM_ENABLE
    /* Same reason as MQTT: not while a diag session forces the tags. */
    if (!suspended) {
#if SPLC_MBW_ENABLE
        plc_mbw_scan(now_ms);    /* BEFORE plc_mbm_scan(): a write waiting for a port goes before its reads */
#endif
        plc_mbm_scan(now_ms);
    }
#endif
    modbus_config_service();
    plc_wifi_service(now_ms);    /* no-op unless SPLC_WIFI_ENABLE: boot restore + save after GOT_IP */
    plc_mqtt_tls_service(now_ms);/* no-op unless SPLC_MQTT_TLS_CERT_ENABLE: abandon a certificate transfer idle for 30 s */
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