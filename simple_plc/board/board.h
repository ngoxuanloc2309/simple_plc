#ifndef BOARD_H
#define BOARD_H

/*
 * board.h - Layer 4 (Engine & Application entry)
 *
 * board_init(): common entry point, same call for every SKU. Calls
 * board_hw_init() internally to do the SKU-specific pin wiring.
 *
 * board_hw_init(): implemented once per SKU (e.g. board_zigbee_io.c,
 * future board_remoteio.c) -- registers that board's real DI/DO/AI
 * channels via plc_io_register_di/do/ai() (services/plc_io/plc_io.h).
 * Only one SKU's .c file is compiled into a given firmware image, so
 * there is never more than one definition of board_hw_init() linked at
 * once despite the shared declaration here.
 *
 * Must be called AFTER tag_table_load_from_flash() (so g_tag_table[]
 * already has the right TAG_DI/TAG_DO/TAG_AI kinds for
 * plc_io_register_*() to validate against) and BEFORE plc_engine_init()
 * per docs/architecture.md / docs/handoff.md section 3 item 11.
 */

#include "modbus_transport.h"
#include "plc_tag.h" /* SPLC_TagLayout */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Returns this SKU's tag counts (DI/DO/AI/VFLAG/VREG/VREG_RETAIN/COUNTER),
 * as a plain data query -- unlike board_init()/board_hw_init(), this touches
 * no GPIO/UART/USB/peripheral state at all, so it is safe to call before
 * any hardware is brought up.
 *
 * MUST be called, and its result passed to tag_table_load_from_flash()
 * (core/plc_tag/plc_tag.h, Layer 2), BEFORE board_init() -- board_init()
 * (via board_hw_init()) calls plc_io_register_di/do/ai(), which validates
 * against g_tag_table[]'s kinds; that table only has the right kinds once
 * tag_table_load_from_flash() has already run. See plc_engine_init()
 * (app/plc_app/plc_engine.c) for the exact call order.
 *
 * Each board's board_<sku>.c defines its own layout via
 * board/board_tag_define.h; this function is how Layer 4's plc_engine.c
 * gets that layout without needing to know which concrete board is linked
 * in -- same "board owns the number, caller only sees the abstraction"
 * split board_get_modbus_transport() below already established.
 */
SPLC_TagLayout board_get_tag_layout(void);

void board_init(void);
void board_hw_init(void);

/*
 * Returns the modbus_transport_t this SKU's App<->MCU config channel
 * runs over -- e.g. board_zigbee_io.c returns
 * modbus_transport_usb_create(&s_board.usb) over its own static USB
 * instance. plc_engine_init() (Layer 4) calls this once, after
 * board_init(), to feed plc_modbus_cfg_init() without ever needing to
 * know which concrete driver instance (sx_usb_tiny_t*, sx_uart_t*, ...)
 * backs it -- same "board owns the instance, caller only sees the
 * abstraction" split board_hw_init() already established for DI/DO via
 * plc_io_register_di/do().
 *
 * Must be called AFTER board_hw_init() (so the underlying driver
 * instance -- e.g. sx_usb_tiny_init() -- is already initialized before
 * anything reads from it).
 */
modbus_transport_t board_get_modbus_transport(void);

#ifdef __cplusplus
}   
#endif

#endif