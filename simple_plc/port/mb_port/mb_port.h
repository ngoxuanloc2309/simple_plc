/*
 * mb_port.h - Layer 3.5 adapter between libs/mb_master_slave (SPLC_MODBUS_LIB_MB)
 * and the product's byte link. Included by plc_modbus_cfg.c (App link) and plc_mbs.c (RS-485 Slave).
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
#include "modbus_serial.h"      /* modbus_serial_port_t */

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

/* --- RS-485 channels: Modbus SLAVE (plc_mbs; needs SPLC_MODBUS_SLAVE_ENABLE) ---
 * Callbacks of the one fixed map. Same shape as mb_port_holding_cb_t; for coils / discrete
 * inputs `nregs` is the number of bits and `buf` is LSB-first packed. Return 0 or a Modbus
 * exception (1, 2, 3, 6; anything else = device failure). `discrete` is never called with write. */
typedef struct {
    mb_port_holding_cb_t holding;
    mb_port_holding_cb_t coils;
    mb_port_holding_cb_t discrete;
} mb_port_slave_ops_t;

/* Open a slave server (unit `slave_id`) on `port`; `framing` is SPLC_MODBUS_FRAMING_RTU or
 * _ASCII (ASCII needs the port built for it: SPLC_MBM_PORTn_FRAMING). `baud` only sets T3.5
 * (RTU). Returns the channel, or -1 (logged). Poll it once per engine scan; close before
 * reopening with another id, baud or framing. The ops are shared by all Slave ports (one map). */
int  mb_port_slave_open(const modbus_serial_port_t *port, uint32_t baud, uint8_t slave_id,
                        uint8_t framing, const mb_port_slave_ops_t *ops);
void mb_port_slave_poll(int ch);
void mb_port_slave_close(int ch);

/* --- RS-485 channels: Modbus MASTER over a modbus_serial_port_t ---
 * Used by port/modbus_serial/modbus_serial_mb.c only (LIB == MB). Same eMBP* layer as the
 * App link; a channel is bound to a port and the library gets its index as `ucPort`.
 * Order per tick (86): pump (RX bytes, then timers) -> library poll -> flush (frame out). */
int  mb_port_serial_attach(const modbus_serial_port_t *port, uint32_t baud);   /* channel, or -1 */
void mb_port_serial_set_baud(int ch, uint32_t baud);
void mb_port_serial_detach(int ch);
void mb_port_serial_pump(int ch);
void mb_port_serial_flush(int ch);
void mb_port_serial_drain(int ch);

/* --- TCP master channels (SPLC_MODBUS_TCP_MASTER_ENABLE) ---
 * Used by port/modbus_tcp_master/modbus_tcp_master.c only. The HOST owns the socket: the
 * channel sees it through three callbacks, so the library's TCP client (mbmtcp.c) runs on a
 * connection somebody else opened. Connecting is the facade's job (it asks the host); the
 * library's eMBPTCPClientOpen() only attaches to the connection that is already up.
 * Order per tick (86): pump (RX bytes into the library, then its timers) -> library poll. */
typedef struct {
    int  (*read)(void *ctx, uint8_t *buf, uint16_t max_len);  /* >=0 bytes (0 = none yet), <0 lost */
    int  (*write)(void *ctx, const uint8_t *buf, uint16_t len); /* bytes taken; != len = lost */
    void (*close)(void *ctx);                                  /* idempotent */
    void *ctx;
} mb_port_tcpc_ops_t;

int  mb_port_tcpc_attach(const mb_port_tcpc_ops_t *ops);   /* channel, or -1 (logged); ops must outlive it */
void mb_port_tcpc_detach(int ch);
/* Call right before eMBMTCPInit() (eMBPTCPClientInit has no channel argument); -1 right after. */
void mb_port_tcpc_bind(int ch);
void mb_port_tcpc_pump(int ch);
bool mb_port_tcpc_is_up(int ch);      /* library client open and the connection has not failed since */
bool mb_port_tcpc_take_lost(int ch);  /* true once if the connection was lost since the last call */

#ifdef __cplusplus
}
#endif

#endif /* SPLC_MB_PORT_H */