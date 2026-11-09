#ifndef PLC_ZIGBEE_H
#define PLC_ZIGBEE_H

/*
 * plc_zigbee.h - Layer 3 (PLC Application Services)
 *
 * Zigbee port configuration (SPLC-SPEC-ZIGBEE-006-ICD v2.0.0, docs/SPLC_SPEC_ZIGBEE_006_ICD.md;
 * decision 98 in docs/handoff.md). Zigbee is a wireless transport that carries Modbus (a DTU
 * Coordinator is the Master, the IO boards are Routers = Slaves); it is NOT a way to
 * load rules. Like the Wi-Fi and TLS blocks this library only does the LOGIC and HOLDS
 * the configuration: there is no radio driver here. The host's PORT
 *   - registers itself with splc_zigbee_register() (this is what makes the block
 *     appear: capability 0x8000 of 0x0A08 is only declared after that),
 *   - reads the running configuration with splc_zigbee_get_config() and applies it to
 *     the module when `generation` changes (it may take seconds, may restart the module),
 *   - reports LINK_STATE / RSSI / LQI / counters / ERROR_CODE through the status callback.
 *
 * Wire (FC03 / FC06 / FC16, 64 registers at 0x1700, big-endian; ICD section 3):
 *   +0   CONTROL_FLAGS  bit 0 ENABLE, bit 1 ROLE (0 Master, 1 Slave), bit 2 FRAMING
 *                       (0 RTU; 1 = ASCII is NOT supported, rejected at COMMIT),
 *                       bit 3 SECURITY_ENABLE, bit 4 ANTENNA (0 PCB, 1 external),
 *                       bit 5 PRESERVE_KEY (a COMMAND: never stored, reads back 0),
 *                       bits 6..15 must be 0
 *   +1   CHANNEL 11..26         +2  PAN_ID 0x0001..0xFFFE
 *   +3   LOCAL_ADDR 0..0xFFFD   +4  DEST_ADDR (any; 0xFFFF = broadcast)
 *   +5   POWER_LEVEL 0..4       +6  BAUD_RATE_IDX 0..4 (9600..115200, module UART)
 *   +7   PARITY 0..2            +8  DATA_BITS (8)       +9  STOP_BITS 1..2
 *   +10  RESPONSE_TIMEOUT_MS 50..5000   +11 INTER_FRAME_DELAY_MS 10..500   +12 RETRY_COUNT 0..5
 *   +13  SLAVE_ID 1..247 (only for ROLE = Slave; 236 / 237 are the PORT's to refuse, below)
 *   +14..+15 RESERVED   stored and read back as written, not checked
 *   +16..+23 NET_KEY    16 bytes, high byte first, WRITE-ONLY (reads back 0)
 *   +24..+31 NETWORK_NAME  16 ASCII bytes, at most 15 characters + NUL
 *   +32..+35 RESERVED   writes are ACKed and ignored, read 0 (not stored: the Flash image
 *                       keeps its 32 registers)
 *   +36 STATUS_FLAGS  bit 0 KEY_SET (the library knows this one: a key is stored)
 *   +37 LINK_STATE  +38 RSSI_DBM (int16)  +39 LQI  +40 TX_PACKETS  +41 RX_PACKETS
 *   +42 TX_ERRORS   +43 RX_ERRORS         +44 ERROR_CODE (PLC_ZIGBEE_ERR_*)   -- from the port
 *   +45..+63 RO 0. Writes to +32..+63 are ACKed and ignored.
 *
 * The RF / UART / name fields (+1..+12, +24..+31) and SLAVE_ID are checked at COMMIT ONLY while
 * ENABLE = 1: a disabled block is stored verbatim, so a host can park any content in it.
 *
 * Staging (same idea as plc_mqtt_sub): host writes to +0..+31 go to a DRAFT (copied from
 * the running block on the first write, so a partial write keeps the rest -- except the
 * key, see below) and are checked at COMMIT:
 *     plc_zigbee_validate_draft() : false = reject the COMMIT, nothing changed
 *     plc_zigbee_commit_draft()   : the draft becomes the running configuration
 *     plc_zigbee_discard_draft()  : drop the draft
 * A Deploy that does not write the block keeps the running configuration (and its key).
 * Reads always return the RUNNING configuration.
 *
 * COMMIT rejects (CONFIG_ERROR_CODE 2) when: FRAMING = 1; a bit 6..15 of CONTROL_FLAGS
 * is set; ENABLE = 1 and the port is not registered or does not support the role asked
 * for; ENABLE = 1 and a field above is out of its range (channel, PAN, addresses, power, UART
 * index, parity, data/stop bits, timeouts, retries, NETWORK_NAME not printable ASCII or not
 * NUL-terminated, SLAVE_ID outside 1..247 for a Slave); a key rule below fails; or the
 * port's `validate` hook (if any) returns false.
 *
 * Network key rules at COMMIT (spec section 5):
 *   SECURITY = 0            PRESERVE_KEY and NET_KEY are ignored, the stored key is kept
 *   SECURITY = 1, PRESERVE  the stored key is kept; none stored => reject
 *   SECURITY = 1, no PRESERVE  the draft MUST carry a new, non-zero NET_KEY => else reject
 * The draft starts with an all-zero key, so "not written" is told apart from "written the
 * same". Consequence: with SECURITY = 1 a partial write (say SLAVE_ID only) needs
 * PRESERVE_KEY = 1 in the same COMMIT. The key is stored in Flash as written (like the
 * MQTT broker password) -- this is not memory-layer security (SPLC-SPEC-SECURITY-001).
 * "A key is stored" means "the stored key is not all zero".
 *
 * Slave ID 236 / 237 (the DRF module treats 0xEC / 0xED as commands) is the PORT's to
 * refuse, not the library's (spec section 8): the port does it in the optional `validate`
 * hook of splc_zigbee_ops_t, which runs at COMMIT after the checks above.
 *
 * Zigbee as a Modbus Master (decision 98): an MB_READ / MB_WRITE block selects the Zigbee pipe with
 * port field PLC_MBM_PORT_ZIGBEE (4 = ICD v2.0.0 "PortIndex = 4", confirmed by the App team).
 * plc_mbm / plc_mbw then treat it as one more Master port (slot after the RS-485 ports): usable only
 * when ENABLE = 1, ROLE = Master and the port registered the MASTER role with a byte pipe
 * (plc_zigbee_master_supported()); otherwise the block reports err_code 1. COMMIT of an enabled
 * block on Zigbee while the (draft or running) role is Slave => CONFIG_ERROR_CODE 7.
 * The Slave endpoint that serves SLAVE-001 on the pipe lives in plc_mbs. MODBUS_MASTER 0x0100 is
 * declared by plc_mbm (MBM = 1); 0x4000 only when the registered port keeps the Slave role.
 *
 * CLEAR_RULES does not touch this block; FACTORY_RESET wipes it, the key included.
 * Saved in the Flash Rule Table record (flag bit 8, SPLC_RULE_FLASH_ZIGBEE_FLAG), 64 B.
 *
 * Threading: single-threaded, engine context only (decision 31). The port reads the
 * configuration from the task that runs plc_engine_poll(), or queues it itself.
 *
 * Compiled only with SPLC_ZIGBEE_ENABLE = 1, so a build with it 0 keeps its exact
 * baseline size.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "splc_opt.h"
#include "modbus_serial.h"   /* modbus_serial_port_t: the byte pipe has the same shape */

#if SPLC_ZIGBEE_ENABLE

#ifdef __cplusplus
extern "C" {
#endif

#define PLC_ZIGBEE_BASE          0x1700U
#define PLC_ZIGBEE_CFG_REGS      32U      /* +0..+31, the part the host writes and Flash keeps */
#define PLC_ZIGBEE_REGS          64U      /* 0x1700..0x173F */
#define PLC_ZIGBEE_FLASH_SIZE    (PLC_ZIGBEE_CFG_REGS * 2U)   /* 64; cross-checked in plc_rule_flash.c */
#define PLC_ZIGBEE_KEY_BYTES     16U

/* Capability bit ZIGBEE of 0x0A08 (App header SPLC_CAP_ZIGBEE). */
#define PLC_ZIGBEE_CAP_ZIGBEE    0x00008000UL

/* CONTROL_FLAGS (+0) */
#define PLC_ZIGBEE_CTRL_ENABLE        0x0001U
#define PLC_ZIGBEE_CTRL_ROLE_SLAVE    0x0002U
#define PLC_ZIGBEE_CTRL_FRAMING_ASCII 0x0004U
#define PLC_ZIGBEE_CTRL_SECURITY      0x0008U
#define PLC_ZIGBEE_CTRL_ANTENNA_EXT   0x0010U
#define PLC_ZIGBEE_CTRL_PRESERVE_KEY  0x0020U

/* STATUS_FLAGS (+36) */
#define PLC_ZIGBEE_STATUS_KEY_SET     0x0001U

#define PLC_ZIGBEE_NAME_BYTES    16U      /* NETWORK_NAME: 15 characters + NUL */

/* ERROR_CODE (+44), ICD v2.0.0 */
#define PLC_ZIGBEE_ERR_NONE            0U
#define PLC_ZIGBEE_ERR_TIMEOUT         1U
#define PLC_ZIGBEE_ERR_INVALID_RF_PARAM 2U
#define PLC_ZIGBEE_ERR_RESTRICTED_ID   3U   /* Slave ID 236 / 237 */
#define PLC_ZIGBEE_ERR_JOIN_FAILED     4U
#define PLC_ZIGBEE_ERR_KEY_REJECTED    5U
#define PLC_ZIGBEE_ERR_HW_FAULT        6U

/* LINK_STATE (+37) values the port reports */
#define PLC_ZIGBEE_LINK_OFF      0U
#define PLC_ZIGBEE_LINK_JOINING  1U
#define PLC_ZIGBEE_LINK_UP       2U
#define PLC_ZIGBEE_LINK_ERROR    3U

typedef enum {
    PLC_ZIGBEE_WRITE_OK = 0,
    PLC_ZIGBEE_WRITE_BAD_ADDRESS       /* -> Modbus 0x02 */
} plc_zigbee_write_result_t;

/* ---- host (port) side ------------------------------------------------- */

#define SPLC_ZIGBEE_ROLE_SLAVE   0x01U   /* the port can be a Router = Modbus Slave */
#define SPLC_ZIGBEE_ROLE_MASTER  0x02U   /* the port can be the Coordinator = Modbus Master */

/* Decoded configuration handed to the port: the RUNNING one (splc_zigbee_get_config) or, in
 * the `validate` hook, the DRAFT as a COMMIT would store it. For the draft `net_key` / `key_set`
 * are the key that WOULD be stored (the new one, or the stored one when it is kept). */
typedef struct {
    bool     enable;
    bool     role_slave;     /* false = Master (Coordinator) */
    bool     security;
    bool     antenna_ext;
    uint16_t channel;
    uint16_t pan_id;
    uint16_t local_addr;
    uint16_t dest_addr;
    uint16_t power_level;
    uint16_t baud_rate_idx;
    uint16_t parity;
    uint16_t data_bits;
    uint16_t stop_bits;
    uint16_t response_timeout_ms;
    uint16_t inter_frame_delay_ms;
    uint16_t retry_count;
    uint16_t slave_id;
    char     name[PLC_ZIGBEE_NAME_BYTES];   /* always NUL-terminated */
    uint8_t  net_key[PLC_ZIGBEE_KEY_BYTES];
    bool     key_set;        /* a (non-zero) key is stored */
} splc_zigbee_config_t;

/* Live values of 0x1725..0x172C, filled by the port. Must not block. The library supplies
 * STATUS_FLAGS (+36) itself. */
typedef struct {
    uint16_t link_state;     /* PLC_ZIGBEE_LINK_* */
    int16_t  rssi_dbm;       /* dBm; 0 = unknown */
    uint16_t lqi;            /* 0..255; 0 = unknown */
    uint16_t tx_packets;
    uint16_t rx_packets;
    uint16_t tx_errors;
    uint16_t rx_errors;
    uint16_t error_code;     /* PLC_ZIGBEE_ERR_*, 0 = none */
} splc_zigbee_status_t;

typedef struct {
    void   *ctx;
    uint8_t roles;           /* SPLC_ZIGBEE_ROLE_* the port supports; at least one bit */
    /* optional; return false = no information (all zero is shown) */
    bool  (*status)(void *ctx, splc_zigbee_status_t *out);
    /*
     * optional. Called at COMMIT with the decoded DRAFT that is about to become the running
     * configuration, AFTER the library's own checks passed (and only when the draft exists).
     * Return false = reject the COMMIT (CONFIG_ERROR_CODE 2, nothing changes, every draft is
     * dropped). For what only the port knows: Slave IDs the module reserves (DRF 236 / 237),
     * PAN / channel / antenna limits of the module. It sees the whole draft, so check
     * `enable` (and `role_slave`) before judging a field the draft does not use. Must not
     * block and must not call back into the library. NOT called for a Flash image at boot
     * (plc_zigbee_import): the port has to cope with whatever is stored.
     */
    bool  (*validate)(void *ctx, const splc_zigbee_config_t *draft);
    /*
     * Byte pipe to the module (e.g. the USART that talks to the DRF module), REQUIRED
     * when `roles` has SLAVE. Same contract as modbus_serial_port_t (modbus_serial.h):
     *   read : non-blocking, copy up to max_len bytes, return the count (0 = none).
     *   write: send len bytes, return 0 when done, negative on failure; the module is
     *          transparent and has no RS-485 direction pin, so there is no set_tx and no
     *          echo to filter. Must not block longer than one frame's TX time.
     * The library marks the pipe as "does not keep byte timing" (SPLC_MODBUS_SYNC_LENGTH)
     * itself: RTU frames are cut by length, never by the 3.5-character silence.
     * Both stay NULL for a port that only configures the module.
     */
    int   (*read)(void *ctx, uint8_t *buf, uint16_t max_len);
    int   (*write)(void *ctx, const uint8_t *buf, uint16_t len, uint32_t timeout_ms);
    uint32_t baudrate;       /* real UART speed to the module, for the library's timing */
} splc_zigbee_ops_t;


/* Register the port (kind of netif_add; the struct is copied). Returns false (nothing
 * registered) if ops is NULL or no role bit is set. Call before plc_engine_init() or
 * from board_init(); the Zigbee capability is declared from then on. */
bool splc_zigbee_register(const splc_zigbee_ops_t *ops);

/* The byte pipe registered above, shaped for the Modbus layer (read/write/baudrate, no
 * set_tx, frame_sync = LENGTH). NULL when no port is registered or it gave no pipe.
 * Valid until the next splc_zigbee_register(). For Layer 3 users (the Slave endpoint). */
const modbus_serial_port_t *splc_zigbee_serial_port(void);

/* true when a port is registered that supports the Master role AND gave a byte pipe: the
 * Zigbee pipe can then be used as a Modbus Master port by MB_READ / MB_WRITE (plc_mbm). */
bool plc_zigbee_master_supported(void);

/* true when the block will be an ENABLED Slave once the pending COMMIT is applied (the draft
 * if one was written, else the running block). plc_mbm / plc_mbw use it to refuse an enabled
 * MB_READ / MB_WRITE block that names the Zigbee port (CONFIG_ERROR_CODE 7). */
bool plc_zigbee_is_slave_after_commit(void);

/* The RUNNING configuration. `generation` (may be NULL) changes whenever the running
 * configuration really changes (COMMIT, Flash load, FACTORY_RESET); the port re-applies
 * it to the module then. Returns false (and `out` zeroed) if out is NULL. */
bool splc_zigbee_get_config(splc_zigbee_config_t *out, uint32_t *generation);

/* ---- engine side ------------------------------------------------------ */

/* Boot: block disabled and empty, draft dropped. Call once, before plc_rule_flash_load(). */
void plc_zigbee_init(void);

/* 0x0A08 contribution: PLC_ZIGBEE_CAP_ZIGBEE once a port is registered, else 0. */
uint32_t plc_zigbee_capability(void);

/* FC03 of `quantity` registers at `offset` (0..63): the RUNNING block, key reads 0. */
void plc_zigbee_read(uint16_t offset, uint16_t quantity, uint16_t *registers_out);

/* FC16 (FC06 arrives as quantity 1) into the DRAFT. A request that runs past the end of
 * the area is BAD_ADDRESS and changes nothing. Content is checked at COMMIT. */
plc_zigbee_write_result_t plc_zigbee_write(uint16_t offset, uint16_t quantity,
                                           const uint16_t *registers);

bool plc_zigbee_validate_draft(void);   /* true also when nothing was written */
void plc_zigbee_commit_draft(void);     /* call only after validate returned true */
void plc_zigbee_discard_draft(void);

/* FACTORY_RESET: configuration and key back to default (disabled, no key). */
void plc_zigbee_factory_reset(void);

/* Flash image: PLC_ZIGBEE_FLASH_SIZE bytes = the 32 RUNNING registers, big-endian,
 * PRESERVE_KEY never set. */
void plc_zigbee_export(uint8_t *out);

/* Load an image written by plc_zigbee_export(). Checks only what is wrong in ANY image
 * (FRAMING = 1, bits 5..15 of CONTROL_FLAGS, a Slave ID outside 1..247 while enabled as
 * Slave): if it fails NOTHING is applied and false is returned. The port is not
 * registered yet at boot, so role support is NOT checked here. Drops any draft. */
bool plc_zigbee_import(const uint8_t *in);

#ifdef __cplusplus
}
#endif

#endif /* SPLC_ZIGBEE_ENABLE */

#endif /* PLC_ZIGBEE_H */