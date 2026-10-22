#ifndef PLC_MQTT_SUB_H
#define PLC_MQTT_SUB_H

/*
 * plc_mqtt_sub.h - Layer 3 (PLC Application Services)
 *
 * MQTT Subscribe: remote commands / setpoints from the cloud land in tags
 * (ICD SPLC-SPEC-MQTT-002, docs/firmware/; decision 47 in docs/handoff.md).
 * Like the publish side (plc_mqtt.h) SimplePLC only does the LOGIC and HOLDS the
 * data: there is no network client in this library (decision 31). The host
 *   - subscribes the topics this library asks for (splc_mqtt_sub_get()),
 *   - hands every received message to splc_mqtt_input().
 *
 * Wire (FC03 / FC06 / FC16, 2 blocks x 64 registers = 128 registers at 0x0D80):
 *   +0 flags      bit 0 enabled, bits 1..2 QoS, bit 3 FIFO (else latest only),
 *                 bit 4 ignore retained, bits 5..6 fail-safe action
 *                 (0 keep, 1 reset to 0, 2 default value)
 *   +1 reserved   +2..+3 fail-safe timeout ms (High Word first, 0 = off)
 *   +4 pin count (1..4)   +5..+7 reserved
 *   +8..+23 topic filter, 32 chars, two per register (first char in the high
 *           byte), NUL padded; MQTT wildcards '+' and '#' are allowed
 *   +24 + 10*i  pin i: +0 target tag (0xFFFF = none), +1 data type (1 BOOL,
 *           2 INT32, 3 REAL), +2..+3 default value (High Word first; float32
 *           bits for REAL), +4..+9 key, 12 chars (see "Payload" below)
 * The source of truth for the layout is the App codec MqttSubscriptionCodec.cs
 * + simpleplc_protocol_v2_0.h; the firmware reads REGISTER by REGISTER and never
 * overlays a struct on the wire bytes.
 *
 * Staging (same idea as plc_mbm / plc_mqtt): host writes do NOT touch the
 * running blocks. They go to a DRAFT block (copied from the running block on the
 * first write to it, so a partial write keeps the rest) and are checked at
 * COMMIT:
 *     plc_mqtt_sub_validate_draft() : false = reject the COMMIT, nothing changed
 *     plc_mqtt_sub_commit_draft()   : the drafted blocks become the running blocks
 *     plc_mqtt_sub_discard_draft()  : drop the draft
 * A Deploy that does not write a block keeps it running. The 2 blocks are saved
 * in the Flash Rule Table record (flag bit 10, SPLC_RULE_FLASH_SUB_FLAG).
 *
 * Payload (key of a pin):
 *   ""  or "raw"  the whole payload is the value ("50", "12.5", "true", "ON")
 *   "a.b.c"       JSON path: members by name (case-insensitive, '_' and '-'
 *                 ignored), array elements by "x[0]" / "[0]"; the message must
 *                 start with '{' or '['
 *   "pos[i]"      i-th value of a plain-text list "5,1" (separators , ; tab),
 *                 or element i of a JSON array at the root
 * A value is a number, true/false/on/off, or a string holding one of those.
 * Parsing is zero-allocation and bounded (depth 16, 128-byte payload).
 * ATOMIC (App contract): a message is applied only when EVERY pin with a target
 * tag finds a valid value; otherwise nothing is written and the message is
 * counted as rejected.
 *
 * Writing the tags (same rule as MB_READ, decision 40(b)): the TARGET TAG decides
 * how the value is stored. A tag the tag-type table (plc_tagtype) marks REAL gets
 * the float bits (an integer value is converted); any other tag gets the value
 * rounded to int32. A BOOL pin writes 0 / 1. Without SPLC_REAL_ENABLE a tag cannot
 * hold a float: REAL values are rounded.
 * The kind of the target tag is NOT restricted (decision 47: follows the App,
 * which sets no restriction; DO is allowed). A pin whose target is 0xFFFF is
 * skipped. Only a tag that exists (kind != TAG_NONE) passes COMMIT. To restrict
 * the kinds later, change target_kind_ok() in plc_mqtt_sub.c, nothing else.
 *
 * Queueing: bit 3 = 0 keeps only the LATEST message per block. Bit 3 = 1 queues
 * up to SPLC_MQTT_SUB_FIFO_DEPTH messages per block, one is applied per scan; a
 * message arriving at a full queue is dropped (counted). With depth 1 a FIFO
 * request behaves as latest-only (logged at COMMIT).
 *
 * Fail-safe: when no valid message arrived for `timeout` ms (counted from the
 * block becoming active, then from the last applied message) the action of bit
 * 5..6 is applied ONCE to every target tag of the block; it is armed again by the
 * next applied message, so rules can write the tags in between.
 *
 * Scan order: plc_mqtt_sub_scan() runs after input_scan() and BEFORE the rule
 * scan, so the rules of this scan already see the new values. Not run while a
 * diag session suspends the engine (the tags are forced by the Host then).
 *
 * Threading: single-threaded, engine context only (decision 31). Every function
 * below, splc_mqtt_input() included, must be called from the task that runs
 * plc_engine_poll(); a host whose network callback runs elsewhere queues the
 * message itself and calls splc_mqtt_input() from its pump.
 *
 * Compiled only with SPLC_MQTT_SUB_ENABLE = 1 (needs SPLC_MQTT_ENABLE = 1), so a
 * build with it 0 keeps its exact baseline size.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "splc_opt.h"

#if SPLC_MQTT_SUB_ENABLE

#ifdef __cplusplus
extern "C" {
#endif

#define PLC_MQTT_SUB_BASE          0x0D80U
#define PLC_MQTT_SUB_BLOCKS        2U
#define PLC_MQTT_SUB_BLOCK_REGS    64U
#define PLC_MQTT_SUB_REGS          (PLC_MQTT_SUB_BLOCKS * PLC_MQTT_SUB_BLOCK_REGS)   /* 128 */
#define PLC_MQTT_SUB_FLASH_SIZE    (PLC_MQTT_SUB_REGS * 2U)                          /* 256 */
#define PLC_MQTT_SUB_MAX_PINS      4U
#define PLC_MQTT_SUB_PAYLOAD_MAX   128U     /* bytes; a longer message is dropped */
#define PLC_MQTT_SUB_TOPIC_MAX     32U      /* characters, without the NUL */

/* Capability bit MQTT_SUB of 0x0A08 (App header SPLC_CAP_MQTT_SUB). */
#define PLC_MQTT_SUB_CAP_MQTT_SUB  0x00000020UL

typedef enum {
    PLC_MQTT_SUB_WRITE_OK = 0,
    PLC_MQTT_SUB_WRITE_BAD_ADDRESS     /* -> Modbus 0x02 */
} plc_mqtt_sub_write_result_t;

/* ---- engine side ------------------------------------------------------ */

/* Boot: every block disabled, draft dropped, queues empty. Call once, before
 * plc_rule_flash_load(). */
void plc_mqtt_sub_init(void);

/* FC03 of `quantity` registers at `offset` (0..127): the RUNNING blocks. */
void plc_mqtt_sub_read(uint16_t offset, uint16_t quantity, uint16_t *registers_out);

/* FC16 (FC06 arrives as quantity 1) into the DRAFT. A request that runs past the
 * end of the area is BAD_ADDRESS and changes nothing. Content is checked at
 * COMMIT, not here. */
plc_mqtt_sub_write_result_t plc_mqtt_sub_write(uint16_t offset, uint16_t quantity,
                                               const uint16_t *registers);

bool plc_mqtt_sub_validate_draft(void);   /* true also when nothing was written */
void plc_mqtt_sub_commit_draft(void);     /* call only after validate returned true */
void plc_mqtt_sub_discard_draft(void);

/* Flash image: PLC_MQTT_SUB_FLASH_SIZE bytes = the 128 RUNNING registers,
 * big-endian. */
void plc_mqtt_sub_export(uint8_t *out);

/* Load an image written by plc_mqtt_sub_export(). The whole image is checked with
 * the COMMIT rules; if it fails NOTHING is applied and false is returned. Used at
 * boot and to roll back a failed CLEAR_RULES. Drops any draft and the queues. */
bool plc_mqtt_sub_import(const uint8_t *in);

/* CLEAR_RULES / FACTORY_RESET: every block disabled, draft and queues dropped. */
void plc_mqtt_sub_clear_all(void);

/* One scan step: apply the queued message of every active block, then run the
 * fail-safe watchdogs. now_ms = the scan's tick (unsigned arithmetic, safe
 * across the 32-bit rollover). */
void plc_mqtt_sub_scan(uint32_t now_ms);

/* ---- host side -------------------------------------------------------- */

/* The topic filter and QoS the host must subscribe for block `block`
 * (0..PLC_MQTT_SUB_BLOCKS-1). Returns false when the block is disabled (nothing
 * to subscribe); topic_out needs PLC_MQTT_SUB_TOPIC_MAX + 1 bytes. Any pointer
 * may be NULL. */
bool splc_mqtt_sub_get(uint8_t block, char *topic_out, uint8_t *qos_out);

/* Moves whenever the set of topics may have changed (COMMIT of a block, Flash
 * import, CLEAR_RULES). The host re-subscribes when it differs from the value it
 * saw at the last subscribe, and after every (re)connect. */
uint32_t splc_mqtt_sub_generation(void);

/* Hand one received message to the library. `topic` and `payload` are NOT kept
 * (the payload is copied, at most PLC_MQTT_SUB_PAYLOAD_MAX bytes); `retained` is
 * the retain flag of the PUBLISH as the broker delivered it. Returns true when
 * at least one enabled block's filter matches the topic. */
bool splc_mqtt_input(const char *topic, const char *payload, size_t len, bool retained);

typedef struct {
    uint32_t received;   /* messages that matched a block (counted per block) */
    uint32_t applied;    /* messages that wrote their tags */
    uint32_t rejected;   /* unparsable, or a pin without a valid value (atomic) */
    uint32_t dropped;    /* too long, retained while ignored, queue full */
    uint32_t failsafes;  /* fail-safe actions applied */
} splc_mqtt_sub_stats_t;

void splc_mqtt_sub_get_stats(splc_mqtt_sub_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* SPLC_MQTT_SUB_ENABLE */

#endif /* PLC_MQTT_SUB_H */