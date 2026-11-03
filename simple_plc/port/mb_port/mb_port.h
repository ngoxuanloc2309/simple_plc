/*
 * mb_port.h - Layer 3.5 adapter between libs/mb_master_slave (SPLC_MODBUS_LIB_MB)
 * and the product's byte link. Included ONLY by plc_modbus_cfg.c.
 *
 * Contract (decision 81): the product writes nothing new. The adapter works on
 * the modbus_transport_t that splc_link_get_transport() already gives (read /
 * write / process / kind / unit_id); the library's eMBP* porting functions are
 * implemented in mb_port.c on top of it. A product never writes an eMBP*.
 *
 * Driving order (handoff 86, verified on a host model only): every engine tick
 *   1. transport->process()                     (plc_modbus_cfg.c)
 *   2. feed all pending RX bytes to the library
 *   3. run this slot's expired timers (T3.5 ...)
 *   4. eMBSPoll()
 *   5. flush the reply the library produced to transport->write()
 * Doing 3 before 2 breaks frames. mb_port_server_poll() does 2..5 in this order.
 *
 * TCP (kind == TCP): the transport is the already-accepted connection; the
 * library sees ONE always-connected virtual client per slot (a second
 * connection = a second transport slot). The MBAP framing is the library's; no
 * T3.5 timer. Not supported: Modbus UDP, and the library's statistics /
 * analyzer interfaces.
 *
 * Single context only (engine); no ISR may call anything here.
 */
#ifndef SPLC_MB_PORT_H
#define SPLC_MB_PORT_H

#include <stdbool.h>
#include <stdint.h>
#include "splc_opt.h"
#include "modbus_transport.h"

/* Declarations are NOT conditional on SPLC_MODBUS_LIB: a prototype costs nothing
 * and a stale/odd macro state must never turn the calls into implicit
 * declarations. The functions exist (mb_port.c) only when LIB == MB. */
#ifdef __cplusplus
extern "C" {
#endif

/*
 * Holding-register access as the register map sees it, library independent.
 *   buf    : big-endian register bytes (2 per register), nregs * 2 bytes.
 *            Read: fill it. Write: it holds the new values.
 *   write  : false = FC03, true = FC06/FC16.
 *   return : 0 = OK, otherwise a Modbus exception code (1..4).
 * FC06 arrives as write with nregs == 1 (the library does not tell it apart
 * from FC16 of one register).
 */
typedef uint8_t (*mb_port_holding_cb_t)(uint8_t *buf, uint16_t addr,
                                        uint16_t nregs, bool write);

/*
 * Create the server of transport slot `slot` (0 .. SPLC_MODBUS_MAX_TRANSPORTS-1)
 * on top of `t` (RTU, ASCII or TCP; `t` must outlive the server, it is not copied).
 * Returns false and logs on any failure (bad kind, no server instance, ...).
 */
bool mb_port_server_create(uint8_t slot, const modbus_transport_t *t,
                           mb_port_holding_cb_t holding_cb);

/* One tick of slot `slot` (steps 2..5 above). Does nothing if the slot has no
 * server. Never blocks (transport->read is called with timeout 0). */
void mb_port_server_poll(uint8_t slot);

#ifdef __cplusplus
}
#endif

#endif /* SPLC_MB_PORT_H */