#ifndef PLC_TAG_H
#define PLC_TAG_H

/*
 * plc_tag.h - Layer 2 (PLC Core)
 *
 * Tag Table: unified abstraction for every I/O point and internal variable
 * in the system. This file must not include anything from Layer 0/1
 * (platform or driver headers). This is the porting boundary: Layer 2 must
 * build and unit test on a plain PC toolchain, independent of any real
 * hardware.
 *
 * See docs/architecture.md, section 2, "Layer 2 - PLC Core".
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum number of tags supported system-wide.
 * Per the base spec: 69 tags used by the Remote I/O SKU, 59 slots reserved
 * for future Gateway SKU expansion. */
#define MAX_TAGS 128

/* TagKind identifies what a tag represents. It does not carry any live
 * value; live values live in g_tag_value[] instead. */
typedef enum {
    TAG_NONE = 0,     /* Unused slot */
    TAG_DI,           /* Digital input; only input_scan() may write it */
    TAG_DO,           /* Digital output; output_scan() reads it to drive a pin */
    TAG_AI,           /* Analog input; only input_scan() may write it */
    TAG_VFLAG,        /* Internal virtual flag, no physical pin backing it */
    TAG_VREG,         /* Internal virtual register, not retained across power loss */
    TAG_MB_COIL,      /* Coil on a remote Modbus device (future Gateway SKU) */
    TAG_MB_HOLDING,   /* Holding register on a remote Modbus device */
    TAG_VREG_RETAIN,  /* Virtual register retained through Flash snapshot */
} TagKind;

/*
 * Tag describes the meaning of a single slot. It is metadata only and does
 * not hold a live value. It is loaded once at boot and never changes at
 * runtime.
 *
 * Size: 4 bytes per tag.
 */
typedef struct {
    uint8_t  kind;      /* One of TagKind */
    uint8_t  channel;   /* Local physical channel, unused for VFLAG/VREG */
    uint16_t reg_addr;  /* Only meaningful for TAG_MB_COIL / TAG_MB_HOLDING */
} Tag;

/* Tag metadata table. Populated once at boot, read-only afterwards. */
extern Tag g_tag_table[MAX_TAGS];

/*
 * Live value table. This is the single shared RAM area written and read
 * by input_scan(), rule_scan(), output_scan(), and modbus_config_service().
 * External code must never touch this array directly; all access must go
 * through tag_read()/tag_write().
 */
extern int32_t g_tag_value[MAX_TAGS];

/*
 * Load the tag metadata table (g_tag_table[]) from Flash. Called exactly
 * once at boot, from plc_engine_init() (Layer 4).
 *
 * Implementation note: reading Flash requires a platform-provided function.
 * This file must stay free of any Layer 0/1 include; the actual Flash
 * access is expected to be injected via a function pointer or a thin
 * wrapper supplied from Layer 3/4. See the TODO in plc_tag.c.
 */
void tag_table_load_from_flash(void);

/*
 * Read the current value of a tag. The caller does not need to know, and
 * this function does not check, whether idx refers to a DI, DO, AI, or any
 * other kind.
 *
 * idx: tag index, valid range 0 .. MAX_TAGS - 1
 * returns: the value currently stored in g_tag_value[idx]
 */
int32_t tag_read(uint16_t idx);

/*
 * Write a value to a tag. This only updates RAM and never triggers any
 * further action (pull model, not push model; see docs/architecture.md
 * section 6). Whether this value is later reflected on a physical pin is
 * entirely the responsibility of output_scan() (Layer 3), which runs
 * independently on a fixed schedule.
 *
 * idx:   tag index, valid range 0 .. MAX_TAGS - 1
 * value: new value to store
 */
void tag_write(uint16_t idx, int32_t value);

/*
 * Return the TagKind of a given tag. Used by Layer 3 when it needs to know
 * a tag's meaning before mapping it to hardware (e.g. building the list of
 * tags that are of kind TAG_DO).
 *
 * idx: tag index, valid range 0 .. MAX_TAGS - 1
 * returns: one of TagKind
 */
TagKind tag_get_kind(uint16_t idx);

#ifdef __cplusplus
}
#endif

#endif /* PLC_TAG_H */