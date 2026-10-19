/**
 * @file splc_mqtt_wire.h
 * @brief SimplePLC Wire Profile V2 - MQTT IIoT & Telemetry Modbus Wire Specifications
 * @standard SPLC-SPEC-MQTT-001 / IEC 61131-3 Coexistence
 * @copyright (c) 2026 SynaptiX Company. All rights reserved.
 *
 * All multi-byte integers are serialized in Network Byte Order (Big-Endian)
 * across Modbus Holding Registers (16-bit).
 *
 * All structures strictly comply with SPLC_DeviceDescriptor_t packing standards:
 * - Packed using SPLC_PACKED and #pragma pack(push, 1)
 * - PascalCase typedef naming: SPLC_MqttTelemetryBlock_t, SPLC_MqttBrokerConfig_t
 * - Static assertion size checks at compile-time.
 */

#ifndef SPLC_MQTT_WIRE_H
#define SPLC_MQTT_WIRE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) || defined(__clang__)
  #define SPLC_PACKED __attribute__((packed))
#elif defined(_MSC_VER)
  #define SPLC_PACKED
#else
  #define SPLC_PACKED
#endif

#pragma pack(push, 1)

/* =========================================================================
 * 1. MODBUS HOLDING REGISTER MAP ADDRESSES & LIMITS
 * ========================================================================= */
#define SPLC_MODBUS_REG_MQTT_TELEMETRY_BASE   0x0C00
#define SPLC_MQTT_MAX_TELEMETRY_BLOCKS        4
#define SPLC_MQTT_REGS_PER_BLOCK              96
#define SPLC_MQTT_BYTES_PER_BLOCK             192
#define SPLC_MQTT_MAX_TOPIC_CHARS             32
#define SPLC_MQTT_MAX_TEMPLATE_CHARS          128
#define SPLC_MQTT_MAX_TAGS_PER_BLOCK          8

#define SPLC_MODBUS_REG_MQTT_BROKER_BASE      0x0E00
#define SPLC_MQTT_BROKER_CONFIG_REGS          96
#define SPLC_MQTT_BROKER_CONFIG_BYTES         192
#define SPLC_MQTT_BROKER_HOST_MAX_CHARS       64
#define SPLC_MQTT_BROKER_CRED_MAX_CHARS       32

/* =========================================================================
 * 2. MQTT TELEMETRY BLOCK BITMASKS & ENUMS (Offset +0 in Block)
 * ========================================================================= */
#define SPLC_MQTT_CTRL_ENABLED_BIT            0x0001
#define SPLC_MQTT_CTRL_MODE_MASK              0x0006
#define SPLC_MQTT_CTRL_MODE_SHIFT             1
#define SPLC_MQTT_CTRL_QOS_MASK               0x0018
#define SPLC_MQTT_CTRL_QOS_SHIFT              3
#define SPLC_MQTT_CTRL_RETAIN_BIT             0x0020
#define SPLC_MQTT_CTRL_PAYLOAD_MASK           0x00C0
#define SPLC_MQTT_CTRL_PAYLOAD_SHIFT          6

typedef enum {
    SPLC_MQTT_MODE_ON_CHANGE = 0,
    SPLC_MQTT_MODE_COV       = 1,
    SPLC_MQTT_MODE_PERIODIC  = 2
} SPLC_MqttPublishMode_t;

typedef enum {
    SPLC_MQTT_QOS_0 = 0,
    SPLC_MQTT_QOS_1 = 1,
    SPLC_MQTT_QOS_2 = 2
} SPLC_MqttQos_t;

typedef enum {
    SPLC_MQTT_PAYLOAD_COMPACT_JSON = 0,
    SPLC_MQTT_PAYLOAD_RAW_VALUE    = 1,
    SPLC_MQTT_PAYLOAD_CUSTOM       = 2
} SPLC_MqttPayloadMode_t;

typedef enum {
    SPLC_MQTT_ERR_NONE            = 0,
    SPLC_MQTT_ERR_DNS_FAIL        = 1,
    SPLC_MQTT_ERR_TCP_RESET       = 2,
    SPLC_MQTT_ERR_AUTH_FAIL       = 3,
    SPLC_MQTT_ERR_TIMEOUT         = 4
} SPLC_MqttBrokerErrorCode_t;

/* Backwards compatibility aliases */
typedef SPLC_MqttPublishMode_t  splc_mqtt_publish_mode_t;
typedef SPLC_MqttQos_t          splc_mqtt_qos_t;
typedef SPLC_MqttPayloadMode_t  splc_mqtt_payload_mode_t;

/* =========================================================================
 * 3. MQTT TELEMETRY BLOCK WIRE STRUCTURE (192 Bytes = 96 Holding Registers)
 * ========================================================================= */
typedef struct SPLC_PACKED {
    /* Offset +0: Control Flags */
    uint16_t control_flags;

    /* Offset +1: Enable Tag Index (0xFFFF = Always enabled) */
    uint16_t en_tag_index;

    /* Offset +2..3: Periodic Interval (ms), 32-bit Big-Endian (High Word first) */
    uint32_t interval_ms;

    /* Offset +4..5: COV Deadband (IEEE 754 Float32, High Word first) */
    float    deadband;

    /* Offset +6: Tag count bound to this block (1..8) */
    uint16_t tag_count;

    /* Offset +7..14: Tag Binding List (Up to 8 Tag Indices, 0xFFFF if unused) */
    uint16_t bound_tags[SPLC_MQTT_MAX_TAGS_PER_BLOCK];

    /* Offset +15: Reserved Header (luôn ghi 0x0000 theo SPLC-SPEC-IEC-002) */
    uint16_t reserved_header;

    /* Offset +16..31: MQTT Topic (32 bytes ASCII null-padded, 2 chars/reg Big-Endian) */
    char     topic[SPLC_MQTT_MAX_TOPIC_CHARS];

    /* Offset +32..95: Custom JSON Payload Template (128 bytes ASCII null-padded) */
    char     payload_template[SPLC_MQTT_MAX_TEMPLATE_CHARS];
} SPLC_MqttTelemetryBlock_t;

/* Backwards compatibility aliases */
typedef SPLC_MqttTelemetryBlock_t splc_mqtt_telemetry_block_t;

/* =========================================================================
 * 4. MQTT BROKER CONFIGURATION BITMASKS (Offset +0 at 0x0E00)
 * ========================================================================= */
#define SPLC_BROKER_CTRL_ENABLED_BIT          0x0001
#define SPLC_BROKER_CTRL_USE_TLS_BIT          0x0002
#define SPLC_BROKER_CTRL_CLEAN_SESSION_BIT    0x0004
#define SPLC_BROKER_CTRL_VERSION_MASK         0x0018
#define SPLC_BROKER_CTRL_VERSION_SHIFT        3

/* =========================================================================
 * 5. MQTT BROKER CONFIGURATION WIRE STRUCTURE (192 Bytes = 96 Holding Registers)
 * ========================================================================= */
typedef struct SPLC_PACKED {
    /* Offset +0: Broker Control Flags */
    uint16_t broker_control;

    /* Offset +1: Broker Port (e.g. 1883 or 8883) */
    uint16_t broker_port;

    /* Offset +2: Keep-Alive interval (seconds, default 60) */
    uint16_t keepalive_sec;

    /* Offset +3: Reserved (Always 0x0000) */
    uint16_t reserved_0;

    /* Offset +4..35: Broker Hostname or IPv4/IPv6 (64 bytes ASCII null-padded) */
    char     broker_host[SPLC_MQTT_BROKER_HOST_MAX_CHARS];

    /* Offset +36..51: MQTT Client Identifier (32 bytes ASCII null-padded) */
    char     client_id[SPLC_MQTT_BROKER_CRED_MAX_CHARS];

    /* Offset +52..67: MQTT Username (32 bytes ASCII null-padded) */
    char     username[SPLC_MQTT_BROKER_CRED_MAX_CHARS];

    /* Offset +68..83: MQTT Password (32 bytes ASCII null-padded) */
    char     password[SPLC_MQTT_BROKER_CRED_MAX_CHARS];

    /* Offset +84..95: Live Diagnostic & Status Reflection (Read-Only updated by MCU) */
    uint16_t status_flags;        /* +84: Bit 0: Connected, Bit 1: TLS Active, Bit 2: Auth OK */
    uint16_t connection_latency;  /* +85: Round-trip Ping in ms */
    uint32_t tx_message_count;    /* +86..87: Total telemetry messages sent (High Word first) */
    uint32_t tx_byte_count;       /* +88..89: Total payload bytes transmitted (High Word first) */
    uint16_t last_error_code;     /* +90: SPLC_MqttBrokerErrorCode_t */
    uint16_t reserved_status[5];  /* +91..95: Reserved */
} SPLC_MqttBrokerConfig_t;

/* Backwards compatibility alias */
typedef SPLC_MqttBrokerConfig_t splc_mqtt_broker_config_t;

#pragma pack(pop)

/* Compile-time static assertions for exact memory layout */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(SPLC_MqttTelemetryBlock_t) == 192, "SPLC_MqttTelemetryBlock_t must be 192 bytes (96 regs)");
_Static_assert(sizeof(SPLC_MqttBrokerConfig_t)   == 192, "SPLC_MqttBrokerConfig_t must be 192 bytes (96 regs)");
#endif

#ifdef __cplusplus
}
#endif

#endif /* SPLC_MQTT_WIRE_H */
