#ifndef PLC_IO_REMOTE_H
#define PLC_IO_REMOTE_H

/*
 * plc_io_remote.h - Layer 3 (PLC Application Services)
 *
 * Virtual DI/DO for a Gateway: tags that have no pin on this board but stand
 * for DI/DO of I/O boards (RS485 slaves). The Rule Engine and the App see
 * ordinary TAG_DI / TAG_DO tags; this service moves their values over RS485.
 *
 *   input_scan()  -> plc_io_remote_input_scan()   cached remote DI -> DI tags
 *   output_scan() -> plc_io_remote_output_scan()  DO tags -> RS485 requests
 *
 * Both hooks are called from services/plc_io/plc_io.c, so Layer 4 needs no
 * change. With no node added they do nothing (boards with local I/O).
 *
 * Wiring (done once by the board file, all static / compile-time):
 *
 *      plc_io_remote_add_node(&cfg)                  one call per I/O board
 *      plc_io_remote_map_node(node, di_base, do_base)
 *                                                    bit i of the node <-> tag base+i
 *
 * Going from 4DI/4DO to 8DI/8DO is a data change: raise di_count/do_count of
 * the node (<= 16) and of the board's tag layout; nothing in this file or in
 * the protocol changes. A second I/O board is one more node.
 *
 * Link protocol (slave side: any firmware implementing this register block,
 * Modbus RTU FC03/FC16):
 *
 *   0x0C00 R    DI      bit i = DI i
 *   0x0C01 R    DO      bit i = DO i (read back)
 *   0x0C02 R    STATUS  bit0 LEASE_ALIVE, bit2 FAILSAFE, bit3 RESET
 *   0x0C03 W    MASK    \  do = (do & ~mask) | (value & mask)
 *   0x0C04 W    VALUE   /  written together
 *   0x0C05 R/W  LEASE   ms (0 = off, else 1000..60000)
 *   0x0C06 R/W  SAFE    DO bits forced when the lease expires
 *
 * What the gateway does per node (one request in flight at a time, scheduled
 * across scan cycles, never blocking a cycle):
 *   1. SYNC     FC16 0x0C03 x4 (mask=all, value, lease, safe) -- at start,
 *               after the node comes back online, or when STATUS shows
 *               RESET / FAILSAFE.
 *   2. WRITE    FC16 0x0C03 x2 as soon as a DO tag differs from what the
 *               node was last told (mask = changed bits).
 *   3. REFRESH  the same write with mask = all DO bits, every lease/3 --
 *               keeps the node's lease alive and self-heals any drift.
 *   4. POLL     FC03 0x0C00 x3 every SPLC_REMOTE_POLL_MS -- remote DI + STATUS.
 *
 * Offline: after SPLC_REMOTE_OFFLINE_AFTER consecutive failed requests the
 * node is reported offline (plc_io_remote_node_online()), retried every
 * SPLC_REMOTE_RETRY_MS, and resynchronised when it answers again. While
 * offline its DI tags HOLD their last value (an offline node must not look
 * like a falling edge to ON_FALL rules); rules that care read the node state
 * through plc_io_remote_node_online().
 *
 * Pull model: nothing here reacts to a tag write; output_scan() looks at the
 * tags once per cycle.
 */

#include <stdbool.h>
#include <stdint.h>

#include "modbus_serial.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PLC_IO_REMOTE_MAX_BITS  16U   /* DI or DO bits one node can expose */

typedef struct {
    modbus_rtu_master_t *master;      /* the RS485 bus this node sits on */
    uint8_t   unit_id;                /* slave address 1..247 */
    uint8_t   di_count;               /* 0..16 DI bits used */
    uint8_t   do_count;               /* 0..16 DO bits used */
    uint16_t  lease_ms;               /* 0 = no lease; else 1000..60000 */
    uint16_t  safe_value;             /* DO bits forced when the lease expires */
} plc_io_remote_node_cfg_t;

/* Forgets every node and mapping. Optional (all state starts zeroed). */
void plc_io_remote_reset(void);

/*
 * Adds a node. Returns its index (0..SPLC_REMOTE_MAX_NODES-1) or -1 if the
 * table is full or the config is invalid. cfg is copied.
 */
int plc_io_remote_add_node(const plc_io_remote_node_cfg_t *cfg);

/* bit `bit` of node `node` <-> tag `tag_idx` (must be TAG_DI / TAG_DO).
 * false: bad node/bit, wrong tag kind, bit beyond di_count/do_count, or the
 * bit/tag is already mapped. */
bool plc_io_remote_register_di(uint16_t tag_idx, int node, uint8_t bit);
bool plc_io_remote_register_do(uint16_t tag_idx, int node, uint8_t bit);

/*
 * Maps all of a node's DI bits to tags di_tag_base.., and all its DO bits to
 * do_tag_base... Returns the number of mappings that FAILED (0 = all good).
 */
int plc_io_remote_map_node(int node, uint16_t di_tag_base, uint16_t do_tag_base);

/* true once the node has answered and has not since failed
 * SPLC_REMOTE_OFFLINE_AFTER requests in a row. */
bool plc_io_remote_node_online(int node);

/* Number of nodes added. */
int plc_io_remote_node_count(void);

/* Hooks, called by plc_io.c's input_scan()/output_scan(). */
void plc_io_remote_input_scan(void);
void plc_io_remote_output_scan(void);

#ifdef __cplusplus
}
#endif

#endif /* PLC_IO_REMOTE_H */