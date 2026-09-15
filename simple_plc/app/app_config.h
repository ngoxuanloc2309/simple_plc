#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <stdint.h>

/*
 * Descriptor App read after connection.
 * HW/FW display accordance major.minor.patch, such as 1.1.0 / 1.3.2.
 */
typedef struct {
    uint16_t device_class;        // SPLC_DeviceClass
    uint16_t device_variant;      // Variant enum corresponding to device_class

    uint16_t hw_version_major;    // HW major
    uint16_t hw_version_minor;    // HW minor
    uint16_t hw_version_patch;    // HW patch

    uint16_t fw_version_major;    // FW major
    uint16_t fw_version_minor;    // FW minor
    uint16_t fw_version_patch;    // FW patch

    uint16_t protocol_version;    // Version contract App <-> MCU
    uint16_t rule_format_version; // Version layout/semantic RuleRecord
} SPLC_DeviceDescriptor;          // 20 byte

/* The main reason for the last reset. Only one value at a time. */
typedef enum {
    SPLC_RESET_UNKNOWN   = 0,
    SPLC_RESET_POWER_ON  = 1,
    SPLC_RESET_SOFTWARE  = 2,
    SPLC_RESET_WATCHDOG  = 3,
    SPLC_RESET_BROWNOUT  = 4,
    SPLC_RESET_EXTERNAL  = 5
} SPLC_ResetReason;

/*
 * Health bitmask: Can or multiple flags simultaneously.
 * SPLC_HEALTH_NONE = 0 means no health warnings.
 */
typedef enum{
    SPLC_HEALTH_NONE = 0,
    SPLC_HEALTH_CPU_HIGH = 1<<0,
    SPLC_HEALTH_RAM_HIGH = 1<<1,
    SPLC_HEALTH_SCAN_OVERRUN = 1<<2
} SPLC_HealthFlags;

typedef struct{
    uint32_t uptime_s;              // uptime in seconds since last boot, unit: seconds
    uint16_t reset_reason;          // SPLC_ResetReason
    uint16_t health_flags;          // Bitmask of SPLC_HealthFlags

    uint16_t cpu_load_percent;      // CPU load percentage, 0-100
    uint16_t ram_usage_percent;     // RAM usage percentage, 0-100

    uint32_t scan_time_ms;          // time of last scan, microsecond 
    uint32_t max_scan_time_ms;      // max time of last scan, microsecond
} SPLC_DeviceHealth; // 20byte

#endif    // APP_CONFIG_H