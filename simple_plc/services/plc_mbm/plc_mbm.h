#ifndef PLC_MBM_H
#define PLC_MBM_H

/*
 * plc_mbm.h - Layer 3 (PLC Application Services)
 *
 * MB_READ: the device as a Modbus RTU MASTER that polls sensors on an RS485 bus
 * and copies the answers into tags (ICD SPLC-SPEC-MBM-001, docs/firmware/;
 * decisions 38..41 in docs/handoff.md). Up to 4 read blocks, each one a
 * poll transaction (slave, FC01/02/03/04, start address, count, interval,
 * timeout) with up to 4 pins that decode registers into tags.
 *
 * Wire (FC03 / FC06 / FC16, 4 blocks x 64 registers = 256 registers at 0x1000):
 *   +0 flags (bit 0 = enabled)      +1 (FC << 8) | slave        +2 start address
 *   +3 reg count                    +4..+5 poll_interval_ms     +6..+7 timeout_ms
 *   +8 valid tag   +9 error tag   +10 err_code tag   (0xFFFF = none)
 *   +11 pin count (0..4)   +12 read mode (0 cyclic, 1 on rising edge of a tag)
 *   +13 trigger tag (0xFFFF = none)   (block)  /  port record: +14 role, +15 Slave ID
 *   +16 + 12*i  pin i: tag, reg_offset, data_type (1 BOOL 2 INT16 3 UINT16
 *               4 INT32 5 UINT32 6 REAL), endianness (0 ABCD 1 CDAB 2 BADC
 *               3 DCBA), multiplier (float32, High Word first), label (12 B)
 * The firmware decodes REGISTER by REGISTER (big-endian words, High byte first),
 * it never overlays a struct on the wire bytes. The source of truth for the
 * layout is the App codec ModbusMasterCodec.cs + simpleplc_protocol_v2_0.h.
 *
 * Staging (same idea as plc_mqtt / plc_tagtype): Host writes do NOT touch the
 * running blocks. They go to a DRAFT block (copied from the running block on
 * the first write to it, so a partial write keeps the rest). At COMMIT:
 *     plc_mbm_validate_draft() : false = reject the COMMIT, nothing changed
 *     plc_mbm_commit_draft()   : the drafted blocks become the running blocks
 *     plc_mbm_discard_draft()  : drop the draft
 * A Deploy that does not write a block keeps it running (the App writes 0 into
 * a block it no longer uses, which disables it). The 4 blocks are saved in the
 * Flash Rule Table record (flag bit 12, decision 41).
 *
 * Writing the tags (decision 40(b), App contract 2.9.0):
 *   - REAL pin   : value = float32 * multiplier, stored as float bits in the tag
 *                  (a NaN/Inf reading is dropped, the tag keeps its value).
 *   - integer pin: multiplier 1 -> the raw integer. Otherwise it looks the TARGET
 *                  TAG up in the tag-type table (plc_tagtype): REAL -> float bits
 *                  of raw * multiplier, anything else -> round(raw * multiplier).
 *   - BOOL pin   : 0 / 1.
 *   Without SPLC_REAL_ENABLE a tag cannot hold a float: REAL pins are rounded.
 *   A UINT32 above INT32_MAX wraps into the int32 tag slot.
 * Status tags (all optional): valid = 1 for ONE scan after a good answer;
 * error = 1 while the last poll failed; err_code = 0 none, 1 timeout (also a
 * failed transmit), 2 CRC, 3 slave exception, 4 malformed frame.
 *
 * Target tags must be written by the device and not by hardware: VFLAG, VREG,
 * VREG_RETAIN, MB_COIL or MB_HOLDING (a DI/DO/AI/COUNTER target is refused at
 * COMMIT). This is the firmware's reading of the ICD; the ICD is silent on it.
 *
 * RS-485 ports (SPLC-SPEC-MBM-003, 0x1400..0x143F): the device has
 * SPLC_MBM_PORT_COUNT (1..4) ports, reported at 0x0A0A. The App writes one
 * 16-register record per port (+0 enabled | port number 1..4, +1..+2 baud,
 * +3 parity 0 none / 1 even / 2 odd, +4 (data bits << 8) | stop bits, +5 timeout,
 * +6 inter-frame delay, +7 retries, +8..+13 name). The table goes through the same
 * draft / COMMIT / Flash path as the blocks (saved right after them in the bit-12
 * section). A block names its port in bits 4..7 of its flags register. The firmware
 * applies the line settings of a port with the host's configure() callback (never
 * while a request is in flight), keeps the inter-frame delay, and repeats a request
 * that timed out / failed its CRC / came back broken up to `retries` times before
 * reporting the error. The answer timeout of a request is the one in its block (the
 * port's timeout field is only checked). Until the App writes the table every port
 * is enabled at 9600-8-N-1.
 *
 * Which UART serves which port is the host's business: it calls
 * splc_mbm_set_rs485_port() once per port index. A block on a port the host did not
 * register (or that the App disabled) is never sent: it reports err_code 1.
 *
 * Scheduling: ONE request in flight per port, ports run side by side. A port that
 * can send without blocking (write_start/tx_done in modbus_serial_port_t) costs the
 * scan nothing; a port whose write() blocks is started at most once per scan
 * overall. Blocks of one port are served round-robin. Not run while a diag session
 * suspends the engine (the tags are forced by the Host then).
 *
 * Threading: single-threaded, engine context only. Compiled only with
 * SPLC_MBM_ENABLE = 1, so a build with it 0 keeps its exact baseline size.
 */

#include <stdbool.h>
#include <stdint.h>

#include "splc_opt.h"

#if SPLC_MBM_ENABLE

#include "modbus_serial.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PLC_MBM_BASE          0x1000U
#define PLC_MBM_BLOCKS        4U
#define PLC_MBM_BLOCK_REGS    64U
#define PLC_MBM_REGS          (PLC_MBM_BLOCKS * PLC_MBM_BLOCK_REGS)   /* 256 */
/* RS-485 port table (SPLC-SPEC-MBM-003). */
#define PLC_MBM_PORT_BASE         0x1400U
#define PLC_MBM_PORT_MAX          4U                                   /* wire capacity, not the device's port count */
#define PLC_MBM_PORT_REGS         16U
#define PLC_MBM_PORT_TABLE_REGS   (PLC_MBM_PORT_MAX * PLC_MBM_PORT_REGS)   /* 64 */

/* Flash image: the 256 block registers, then the 64 port-table registers. */
#define PLC_MBM_FLASH_SIZE    ((PLC_MBM_REGS + PLC_MBM_PORT_TABLE_REGS) * 2U)   /* 640 */
#define PLC_MBM_MAX_PINS      4U

/* Capability bit MODBUS_MASTER of 0x0A08 (App header SPLC_CAP_MODBUS_MASTER). */
#define PLC_MBM_CAP_MODBUS_MASTER  0x00000100UL

/* Capability bit MODBUS_SLAVE of 0x0A08 (App header SPLC_CAP_MODBUS_SLAVE, bit 14; 0x1000 is
 * HTTP_CLIENT). Declared statically by a build that has the Slave serving module
 * (SPLC_MODBUS_SLAVE_ENABLE = 1); the App refuses to Deploy a Slave port without it.
 * The module does NOT exist yet, so the option stays 0 and the bit is never declared. */
#define PLC_MBM_CAP_MODBUS_SLAVE   0x00004000UL
#ifndef SPLC_MODBUS_SLAVE_ENABLE
#define SPLC_MODBUS_SLAVE_ENABLE   0
#endif

typedef enum {
    PLC_MBM_WRITE_OK = 0,
    PLC_MBM_WRITE_BAD_ADDRESS      /* -> Modbus 0x02 */
} plc_mbm_write_result_t;

/* Parity as it is on the wire (MBM-003 +3). NOTE: sx_uart uses 1 = odd, 2 = even;
 * a configure() callback maps between the two. */
typedef enum { PLC_MBM_PARITY_NONE = 0, PLC_MBM_PARITY_EVEN = 1, PLC_MBM_PARITY_ODD = 2 } plc_mbm_parity_t;

/* Line settings of one port, as the App set them: baud 1200..921600, parity
 * plc_mbm_parity_t, data_bits 7 or 8, stop_bits 1 or 2. */
typedef struct {
    uint32_t baud;
    uint8_t  parity;
    uint8_t  data_bits;
    uint8_t  stop_bits;
} plc_mbm_serial_t;

/* Host callback that re-configures the UART of one port for the given line
 * settings. Returns 0 when the port now runs with them, negative when it cannot
 * (the firmware logs it, retries about once a second and does not poll that port
 * meanwhile). */
typedef int (*plc_mbm_configure_fn)(void *ctx, const plc_mbm_serial_t *cfg);

/* ---- host side -------------------------------------------------------- */

/* Register RS485 port number `port_index` (0..SPLC_MBM_PORT_COUNT-1; the App's
 * "port + 1") that MB_READ talks through (copied; port->ctx is borrowed).
 * `configure` may be NULL when the host cannot change its UART: the baudrate field
 * of `port` is then the real line speed and the App's line settings are ignored.
 * Give the port write_start / tx_done (modbus_serial.h) to keep the scan free of the
 * frame's transmit time. Call once per port, before the first scan; not from another
 * task. A port never registered leaves its blocks reporting err_code 1. */
void splc_mbm_set_rs485_port(uint8_t port_index, const modbus_serial_port_t *port,
                             plc_mbm_configure_fn configure);

/* ---- engine side ------------------------------------------------------ */

/* Boot: every block disabled, draft dropped, serial settings = defaults from
 * splcopts.h. Call once, before plc_rule_flash_load(). */
void plc_mbm_init(void);

/* Number of RS-485 ports of this device: the value of register 0x0A0A. */
uint16_t plc_mbm_port_count(void);

/* Role of an RS-485 port (SPLC-SPEC-SLAVE-001), set by the App in the port table
 * (bit 9 of +0 and +14 = Slave, +15 = Slave ID). A Slave port is never used as a Master:
 * MB_READ / MB_WRITE blocks on it do not send (err_code 1), and COMMIT refuses an enabled
 * block that names it. Switching the role is done by a COMMIT of the port table. */
typedef enum { PLC_MBM_ROLE_MASTER = 0, PLC_MBM_ROLE_SLAVE = 1 } plc_mbm_role_t;

/* Role of port `idx` as running now (MASTER for a disabled port or an index out of range)
 * and its Slave ID (0 unless the port is an enabled Slave). */
plc_mbm_role_t plc_mbm_port_role(unsigned idx);
uint8_t        plc_mbm_port_slave_id(unsigned idx);

/* Turnaround Response Delay of an enabled Slave port in ms: register +6 of its record, the
 * wait before the Slave answers (0 = answer as soon as the 3.5-character silence allows).
 * The serving module must wait with a timer, never by blocking the scan. 0 for a Master or
 * disabled port. */
uint16_t plc_mbm_port_response_delay_ms(unsigned idx);

/* true when port `idx` will be an ENABLED Slave once the pending COMMIT is applied (the draft
 * port table if one was written, else the running one). plc_mbw uses it to refuse an
 * MB_WRITE block on such a port. false for an index out of range. */
bool plc_mbm_port_is_slave_after_commit(unsigned idx);

/* Called (from the COMMIT / boot context) whenever a port's role or Slave ID changes, with
 * the new values; role is MASTER for a disabled port. The module that serves the Slave
 * side registers here to start or stop listening on the port. NULL = nobody listens. */
typedef void (*plc_mbm_role_listener_fn)(unsigned port_index, plc_mbm_role_t role, uint8_t slave_id);
void plc_mbm_set_role_listener(plc_mbm_role_listener_fn fn);

/* FC03 of `quantity` registers at `offset` (0..255): the RUNNING blocks. */
void plc_mbm_read(uint16_t offset, uint16_t quantity, uint16_t *registers_out);

/* FC16 (FC06 arrives as quantity 1) into the DRAFT. A request that runs past
 * the end of the area is BAD_ADDRESS and changes nothing. Content is checked
 * at COMMIT, not here. */
plc_mbm_write_result_t plc_mbm_write(uint16_t offset, uint16_t quantity, const uint16_t *registers);

/* The port table 0x1400..0x143F (64 registers): FC03 returns the RUNNING table,
 * FC16 / FC06 go to a DRAFT (copied from the running table on the first write)
 * checked at COMMIT together with the blocks. A request running past the end of the
 * area is BAD_ADDRESS and changes nothing. */
void plc_mbm_ports_read(uint16_t offset, uint16_t quantity, uint16_t *registers_out);
plc_mbm_write_result_t plc_mbm_ports_write(uint16_t offset, uint16_t quantity, const uint16_t *registers);

bool plc_mbm_validate_draft(void);   /* true also when nothing was written */
void plc_mbm_commit_draft(void);     /* call only after validate returned true */
void plc_mbm_discard_draft(void);

/* Flash image: PLC_MBM_FLASH_SIZE bytes = the 256 RUNNING block registers then the
 * 64 RUNNING port-table registers, big-endian. */
void plc_mbm_export(uint8_t *out);

/* Load an image written by plc_mbm_export(). The whole image is checked with
 * the COMMIT rules; if it fails NOTHING is applied and false is returned. Used
 * at boot and to roll back a failed CLEAR_RULES. Drops any draft. */
bool plc_mbm_import(const uint8_t *in);

/* CLEAR_RULES / FACTORY_RESET: every block disabled, port table back to its
 * defaults, draft dropped. */
void plc_mbm_clear_all(void);

/* One scan step (after output_scan): collect the answer of the request in
 * flight, then start at most one new request. Never blocks beyond one TX. */
void plc_mbm_scan(uint32_t now_ms);

#if SPLC_MBW_ENABLE
/* ---- shared ports for MB_WRITE (plc_mbw.h) ------------------------------
 *
 * The RS-485 ports, their line settings and their one-request-in-flight rule belong
 * to this module; MB_WRITE borrows a port's master for ONE transaction at a time:
 *   claim   -> OK: *master is idle and yours until you release it; *retries is the
 *              port's retry count. BUSY: a read (or another write) is on the bus, or
 *              the line settings are being applied, or a blocking send already took
 *              this scan's slot; ask again next scan (reads then keep off that port).
 *              UNUSABLE: the host registered no port with that index, or the App
 *              disabled it in the port table.
 *   release -> `sent` = true once a request was started on it (also after its answer
 *              came back); false when you claimed it but sent nothing.
 * You poll the master yourself while you own it. Engine context only. */
typedef enum { PLC_MBM_CLAIM_OK = 0, PLC_MBM_CLAIM_BUSY, PLC_MBM_CLAIM_UNUSABLE } plc_mbm_claim_t;

plc_mbm_claim_t plc_mbm_port_claim(unsigned port_index, modbus_rtu_master_t **master, uint8_t *retries);
void            plc_mbm_port_release(unsigned port_index, bool sent);
#endif

#ifdef __cplusplus
}
#endif

#endif /* SPLC_MBM_ENABLE */
#endif /* PLC_MBM_H */