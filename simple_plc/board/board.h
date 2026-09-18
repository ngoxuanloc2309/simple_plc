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

#ifdef __cplusplus
extern "C" {
#endif

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