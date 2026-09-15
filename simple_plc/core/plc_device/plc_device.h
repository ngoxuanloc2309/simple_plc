#ifndef PLC_DEVICE_H
#define PLC_DEVICE_H

/*
 * plc_device.h - Layer 2 (PLC Core)
 *
 * Device identity and runtime health type definitions, per the official
 * data contract in docs/SimplePLC_App_MCU_Structs_v1.7.md, section 1
 * (DEVICE DESCRIPTOR) and the DEVICE_HEALTH register block (section 8.1,
 * 0x0800-0x0809). This file must not include anything from Layer 0/1
 * (platform or driver headers). This is the porting boundary: Layer 2 must
 * build and unit test on a plain PC toolchain, independent of any real
 * hardware.
 *
 * Scope of this file: struct/enum type definitions only, nothing else.
 * There is deliberately no plc_device.c and no extern g_device_descriptor /
 * g_device_health here. Owning the actual instances and populating them
 * (from Flash-stored HW version, compile-time FW version, board init, and
 * the live scan loop for health) requires calling into Layer 0/1, which
 * Layer 2 is not allowed to do -- the same boundary plc_tag.h documents for
 * tag_table_load_from_flash(). That ownership belongs one layer up (Layer
 * 3/4, e.g. services/ or app/plc_app/plc_engine.c), which is free to
 * #include this header for the type definitions and then declare/populate
 * its own g_device_descriptor / g_device_health instances.
 *
 * This file replaces the earlier drafts left in app/app_config.h and
 * board/board_family.h, which put device-identity data outside the Layer 2
 * porting boundary. Those drafts have been removed; this is the single
 * source for SPLC_DeviceClass / SPLC_*Variant / SPLC_DeviceDescriptor /
 * SPLC_DeviceHealth.
 *
 * See docs/architecture.md, section 2, "Layer 2 - PLC Core".
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Device class: top-level product family.
 *
 * NOTE: this used to be modeled as compile-time #define / #ifdef in the
 * removed board/board_family.h draft ("#ifdef SPLC_DEVICE_CLASS_REMOTE_IO").
 * That does not work: the macro was always #define'd (just to 0 or 1), so
 * #ifdef was always true and every variant enum got compiled in at once,
 * regardless of which SKU the firmware was built for. SPLC_DeviceClass is
 * runtime data carried in SPLC_DeviceDescriptor.device_class, set once by
 * the concrete board's init code (Layer 4) -- it does not need, and must
 * not use, conditional compilation.
 */
typedef enum {
    SPLC_DEVICE_CLASS_UNKNOWN    = 0,
    SPLC_DEVICE_CLASS_REMOTE_IO  = 1,
    SPLC_DEVICE_CLASS_DATALOGGER = 2,
    SPLC_DEVICE_CLASS_GATEWAY    = 3,
    SPLC_DEVICE_CLASS_CONTROLLER = 4
} SPLC_DeviceClass;

/* Remote I/O variant: only meaningful when device_class == SPLC_DEVICE_CLASS_REMOTE_IO. */
typedef enum {
    SPLC_REMOTE_IO_VARIANT_UNKNOWN     = 0,
    SPLC_REMOTE_IO_VARIANT_8DI_8DO_4AI = 1,
    SPLC_REMOTE_IO_VARIANT_16DI_16DO   = 2
} SPLC_RemoteIoVariant;

/* Datalogger variant: only meaningful when device_class == SPLC_DEVICE_CLASS_DATALOGGER. */
typedef enum {
    SPLC_DATALOGGER_VARIANT_UNKNOWN = 0,
    SPLC_DATALOGGER_VARIANT_8AI     = 1
} SPLC_DataloggerVariant;

/* Gateway variant: only meaningful when device_class == SPLC_DEVICE_CLASS_GATEWAY. */
typedef enum {
    SPLC_GATEWAY_VARIANT_UNKNOWN   = 0,
    SPLC_GATEWAY_VARIANT_RS485_ETH = 1
} SPLC_GatewayVariant;

/*
 * Descriptor the App reads right after connecting. Read-only from the App's
 * point of view; not user configuration.
 *
 * HW/FW versions display as major.minor.patch, e.g. 1.1.0 / 1.3.2.
 *
 * Wire size: 20 bytes (10 x uint16_t). Mirrors
 * docs/SimplePLC_App_MCU_Structs_v1.7.md section 1 exactly -- do not reorder
 * or resize fields without updating that document first.
 */
typedef struct {
    uint16_t device_class;         /* SPLC_DeviceClass */
    uint16_t device_variant;       /* Variant enum matching device_class (SPLC_RemoteIoVariant / SPLC_DataloggerVariant / SPLC_GatewayVariant) */

    uint16_t hw_version_major;     /* HW major */
    uint16_t hw_version_minor;     /* HW minor */
    uint16_t hw_version_patch;     /* HW patch */

    uint16_t fw_version_major;     /* FW major */
    uint16_t fw_version_minor;     /* FW minor */
    uint16_t fw_version_patch;     /* FW patch */

    uint16_t protocol_version;     /* App <-> MCU contract version */
    uint16_t rule_format_version;  /* SPLC_RuleRecord layout/semantic version */
} SPLC_DeviceDescriptor; /* 20 bytes */

/* Main reason for the last reset. Only one value at a time (not a bitmask). */
typedef enum {
    SPLC_RESET_UNKNOWN  = 0,
    SPLC_RESET_POWER_ON = 1,
    SPLC_RESET_SOFTWARE = 2,
    SPLC_RESET_WATCHDOG = 3,
    SPLC_RESET_BROWNOUT = 4,
    SPLC_RESET_EXTERNAL = 5
} SPLC_ResetReason;

/*
 * Health bitmask: multiple flags may be OR'd together.
 * SPLC_HEALTH_NONE (0) means no health warnings are active.
 */
typedef enum {
    SPLC_HEALTH_NONE         = 0,
    SPLC_HEALTH_CPU_HIGH     = 1 << 0,
    SPLC_HEALTH_RAM_HIGH     = 1 << 1,
    SPLC_HEALTH_SCAN_OVERRUN = 1 << 2
} SPLC_HealthFlags;

/*
 * Runtime health snapshot, updated continuously by Layer 3/4 and exposed
 * read-only over Modbus at DEVICE_HEALTH (0x0800-0x0809).
 *
 * Unit confirmed: scan_time_ms / max_scan_time_ms are milliseconds, matching
 * the field name. (The source docx, SimplePLC_App_MCU_Structs_v1.7, had a
 * stray "microsecond" comment next to the _ms name, and docs/architecture.md
 * separately named these fields scan_time_us / max_scan_time_us -- both were
 * inconsistent with the field name itself. Confirmed with the spec owner:
 * milliseconds is correct: docs/architecture.md's *_us naming should be
 * treated as stale.)
 *
 * Wire size: 20 bytes.
 */
typedef struct {
    uint32_t uptime_s;          /* Seconds since last boot */
    uint16_t reset_reason;      /* SPLC_ResetReason */
    uint16_t health_flags;      /* Bitmask of SPLC_HealthFlags */

    uint16_t cpu_load_percent;  /* 0-100 */
    uint16_t ram_usage_percent; /* 0-100 */

    uint32_t scan_time_ms;      /* Last scan duration, in milliseconds */
    uint32_t max_scan_time_ms;  /* Max scan duration observed, in milliseconds */
} SPLC_DeviceHealth; /* 20 bytes */

typedef struct {
    uint16_t wire_profile;          // SPLC_WireProfile; V1 = 1
    uint16_t max_rules;             // 0..100; >0 => có Rule Engine
    uint16_t runtime_tag_count;     // Tổng tag hợp lệ; không có nghĩa index 0..N-1 liên tục

    uint16_t di_count;              // 0..8
    uint16_t do_count;              // 0..8
    uint16_t ai_count;              // 0..4
    uint16_t vflag_count;           // 0..32
    uint16_t vreg_count;            // 0..32
    uint16_t vreg_retain_count;     // 0..32; >0 => có Retentive Memory
    uint16_t counter_count;         // 0..8
} SPLC_DeviceResourceInfo;          // CHANGED V1.9: 20 byte = 10 Modbus registers


#ifdef __cplusplus
}
#endif

#endif /* PLC_DEVICE_H */