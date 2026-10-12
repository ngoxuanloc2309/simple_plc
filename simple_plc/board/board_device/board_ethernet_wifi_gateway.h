#ifndef ETHERNET_WIFI_GATEWAY_H
#define ETHERNET_WIFI_GATEWAY_H

/*
 * board_ethernet_wifi_gateway.h - Ethernet/WiFi Gateway SKU
 *
 * This board has NO local I/O pins. Its DI/DO are VIRTUAL: each one stands
 * for a DI/DO of an I/O board on the RS485 bus (see services/plc_io/
 * plc_io_remote.h). It owns no peripheral: the RS485 port
 * and the App link are plugged in by the host firmware (see below).
 *
 * ---- Adding / growing I/O boards (the ONLY place to edit) ---------------
 *
 * One row per I/O board:  X(unit_id, di_count, do_count)
 *
 *   4DI/4DO board, address 1 :   X(1, 4, 4)
 *   later 8DI/8DO, address 1 :   X(1, 8, 8)              (just change the row)
 *   plus a second board      :   X(1, 8, 8)  X(2, 4, 4)  (add a row)
 *
 * Virtual tags follow the rows in order: node 0's DI bits become DI tags
 * 0.., node 1's continue after them; the same for DO. The tag layout
 * (GATEWAY_FAKE_IO_DI_NUM / _DO_NUM below) is computed from the rows, so
 * there is nothing else to keep in sync. Limits: <= 16 DI and <= 16 DO per
 * row, <= SPLC_REMOTE_MAX_NODES rows, total tags <= MAX_TAGS
 * (board_tag_define.h) -- all checked at compile time.
 */

#include "splc_opt.h"

#define GATEWAY_IO_NODES(X) \
    X(1, 4, 4)

/* Lease the I/O boards get (ms, 0 = none) and their DO state when it expires
 * (bit i = DO i; 0 = all OFF). Same for every board. */
#define GATEWAY_NODE_LEASE_MS     3000
#define GATEWAY_NODE_SAFE_VALUE   0x0000

/* --- derived totals (do not edit) -------------------------------------- */
#define GW_SUM_DI_(unit, di, do_)  + (di)
#define GW_SUM_DO_(unit, di, do_)  + (do_)
#define GW_SUM_N_(unit, di, do_)   + 1

#define GATEWAY_FAKE_IO_DI_NUM    (0 GATEWAY_IO_NODES(GW_SUM_DI_))
#define GATEWAY_FAKE_IO_DO_NUM    (0 GATEWAY_IO_NODES(GW_SUM_DO_))
#define GATEWAY_NODE_COUNT        (0 GATEWAY_IO_NODES(GW_SUM_N_))

/* Same fixed internal tag ranges as every SKU. */
#define GATEWAY_AI_NUM              0
#define GATEWAY_VFLAG_NUM           32
#define GATEWAY_VREG_NUM            32
#define GATEWAY_VREG_RETAIN_NUM     32
#define GATEWAY_COUNTER_NUM         8

/* --- RS485 port to the I/O boards (supplied by the host firmware) ----------
 *
 * This board owns NO peripheral. The RS485 UART, its driver-enable pin and
 * its baud rate belong to the firmware that embeds SimplePLC (the host); it
 * plugs them in as a modbus_serial_port_t (read / write / set_tx / baudrate,
 * see port/modbus_serial/modbus_serial.h) -- the same way the App link is
 * plugged in with splc_link_register(). The I/O boards must run the same
 * baud rate as port->baudrate, 8N1. */
#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include "modbus_serial.h"

/*
 * Register the RS485 port. The struct is copied; port->ctx is borrowed. Call
 * it BEFORE plc_engine_init() (board_hw_init() builds the virtual I/O from
 * it). Returns false when port->read/write is NULL. Without a registered port
 * the gateway still runs, but every virtual DI/DO stays unserved and an error
 * is logged.
 */
bool splc_gateway_set_rs485_port(const modbus_serial_port_t *port);

#ifdef __cplusplus
}
#endif

/* Modbus RTU unit ID the gateway itself would answer to on its App link. */
#define MODBUS_UNIT_ID            SPLC_MODBUS_UNIT_ID

#endif