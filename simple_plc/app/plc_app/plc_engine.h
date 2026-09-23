#ifndef PLC_ENGINE_H
#define PLC_ENGINE_H

/*
 * plc_engine.h - Layer 4 (Engine & Application entry)
 *
 * The single, SKU-agnostic entry point every board_*.c's firmware image
 * uses the same way. plc_engine.c never knows or checks which SKU it is
 * running on -- see board.h's doc-comment: board_init()/board_hw_init()/
 * board_get_modbus_transport() are the only per-SKU hooks it calls
 * through, and only one SKU's board_<n>.c is ever linked into a given
 * image (selected by SPLC_BOARD_SKU in board/CMakeLists.txt, not by any
 * #ifdef inside this file -- see docs/handoff.md section 1.11's
 * rationale against #ifdef-based SKU selection, and
 * docs/architecture.md's Gateway/Datalogger multi-product discussion).
 */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * One-time setup, called once from main() (Core/Src/main.c, inside
 * USER CODE BEGIN 2 -- after every MX_*_Init() call, so all HAL
 * peripherals board_hw_init() depends on are already configured).
 *
 * Order (see docs/architecture.md section 4's plc_engine_init()
 * ordering, and the individual doc-comments on each function called
 * here for why this exact order matters):
 *   1. tag_table_load_from_flash()   -- g_tag_table[] must have real
 *      TAG_DI/TAG_DO/TAG_AI kinds before board_init() calls
 *      plc_io_register_di/do/ai(), which validate against it.
 *   2. rule_table_load_from_flash()  -- so DEVICE_RESOURCE_INFO's
 *      max_rules/RULE_TABLE_INFO's rule_count already reflect real data
 *      once plc_modbus_cfg_init() (step 5) starts serving Modbus reads.
 *   3. retain_store_restore()        -- restores TAG_VREG_RETAIN values
 *      from their Flash snapshot before rule_scan() ever runs (step 5 of
 *      plc_engine_scan_once() below) reads a stale/zeroed value.
 *   4. board_init()                  -- per-SKU pin wiring
 *      (plc_io_register_di/do/ai()), plus initializing whatever driver
 *      instance (sx_usb_tiny_t, ...) board_get_modbus_transport() (step 5)
 *      will hand off.
 *   5. plc_modbus_cfg_init(board_get_modbus_transport()) -- must run
 *      after board_init(), since the transport's underlying driver
 *      instance is only initialized there (see board.h's doc-comment on
 *      board_get_modbus_transport()'s calling order requirement).
 */
void plc_engine_init(void);

/*
 * Scan period in milliseconds: how far apart plc_engine_poll() starts
 * consecutive scan cycles. Single source of truth for the firmware -- main.c
 * and every other caller use plc_engine_poll() and never carry their own
 * copy of this number.
 *
 * Overridable per build without editing this file, e.g. for bring-up:
 *     target_compile_definitions(<target> PRIVATE PLC_SCAN_INTERVAL_MS=1U)
 * (0 = scan as fast as the loop spins; not recommended -- it floods the log
 * and wastes CPU without improving rule timing.)
 *
 * Rule dwell/interval resolution equals this period (see rule_scan() in
 * plc_rule.h).
 */
#ifndef PLC_SCAN_INTERVAL_MS
#define PLC_SCAN_INTERVAL_MS 10U
#endif

/*
 * Call from main()'s while(1) loop on EVERY iteration. Non-blocking: returns
 * immediately unless at least PLC_SCAN_INTERVAL_MS have passed since the
 * previous cycle started, in which case it runs one full scan cycle. All
 * scan pacing therefore lives in the engine, so a new board/MCU main()
 * needs no timing code of its own. Unsigned delta, so it stays correct
 * across the ~49.7-day tick wraparound.
 */
void plc_engine_poll(void);

/*
 * One PLC scan cycle, unconditionally (no pacing). Normally reached via
 * plc_engine_poll(); call it directly only from a test or a caller that
 * already paces itself.
 *
 * Fixed order (docs/architecture.md section 4.2's scan cycle ordering, and
 * each function's own doc-comment cross-referencing this order):
 *   input_scan() -> rule_scan(now) -> output_scan() ->
 *   modbus_config_service() -> retain_service() -> plc_system_cmd_service()
 *
 * plc_system_cmd_service() runs last on purpose: it may call
 * sx_system_reset() (a pending SPLC_SYSTEM_CMD_REBOOT whose grace period
 * has elapsed), which never returns -- everything else this cycle must
 * already be done first. See plc_system_cmd_service.h for why REBOOT
 * itself is staggered a few cycles past the command that requested it,
 * rather than resetting within the same cycle that first saw it.
 *
 * The tick is sampled ONCE at the start of the cycle and that single value
 * is handed to rule_scan(), so every rule in the cycle shares one "now".
 *
 * Scan duration is timed around this exact sequence and reported via
 * plc_modbus_cfg_record_scan_time() so DEVICE_HEALTH.scan_time_ms/
 * max_scan_time_ms (Modbus-readable) reflect real cycle timing.
 */
void plc_engine_scan_once(void);

#ifdef __cplusplus
}
#endif

#endif /* PLC_ENGINE_H */