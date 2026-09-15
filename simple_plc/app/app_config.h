#ifndef APP_CONFIG_H
#define APP_CONFIG_H

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

#endif    // APP_CONFIG_H