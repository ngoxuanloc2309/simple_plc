#ifndef MODBUS_TRANSPORT_H
#define MODBUS_TRANSPORT_H

/*
 * modbus_transport.h - Layer 3.5 (Protocol / Library Porting)
 *
 * Transport-agnostic interface between services/plc_modbus_cfg.c (Layer 3)
 * and whichever concrete byte transport nanoMODBUS is actually running
 * over (USB-CDC today, RS485/UART or Modbus TCP later for the Gateway
 * product line).
 *
 * Problem this solves: plc_modbus_cfg_init() used to take a
 * sx_usb_tiny_t* directly and plc_modbus_cfg.c used to include
 * sx_usb_cdc.h and call sx_usb_tiny_process() by name. That worked for
 * the Remote I/O SKU, but ties Layer 3's only Modbus config service to
 * one specific Layer 1 driver. Adding a second product (Gateway, over
 * RS485/UART, or later Modbus TCP over Ethernet) would have required
 * either duplicating plc_modbus_cfg.c per transport or reintroducing a
 * compile-time macro switch inside it -- both rejected for the same
 * reason board-level SKU selection was: the concrete transport is a
 * link-time/board-init decision, not something plc_modbus_cfg.c should
 * know about.
 *
 * Design: this struct is intentionally as thin as possible. Its read/
 * write function pointers match nmbs_platform_conf.read/write
 * (libs/nanomodbus/nanomodbus.h) byte for byte, so plc_modbus_cfg_init()
 * can assign them directly with no adapter/wrapper indirection at call
 * time. ctx is opaque here on purpose: this header must not include any
 * Layer 1 driver header (sx_usb_cdc.h, sx_uart.h, ...), so it stays
 * usable from plc_modbus_cfg.c without pulling in a specific driver's
 * types.
 *
 * Per docs/architecture.md's include rules, each concrete transport
 * (port/modbus_usb/, port/modbus_serial/, port/modbus_tcp/) owns exactly
 * one factory function that builds a modbus_transport_t bound to its own
 * driver instance (see modbus_transport_usb_create() in
 * port/modbus_usb/modbus_usb.h for the first one). plc_modbus_cfg.c only
 * ever consumes the resulting modbus_transport_t; it never calls a
 * transport's factory function itself -- that call belongs at board
 * init (Layer 4), which is the only place that knows which physical
 * transport this board build is using.
 *
 * Ownership: the modbus_transport_t value itself is small enough to copy
 * by value (plc_modbus_cfg_init() takes a const pointer only to avoid an
 * unnecessary struct copy at the call site, then stores its own copy
 * internally). ctx is never owned by this struct or by
 * plc_modbus_cfg.c -- the concrete driver instance (sx_usb_tiny_t,
 * sx_uart_t, ...) must be allocated and initialized by the caller (board
 * init) and must outlive every future modbus_config_service() call, same
 * lifetime requirement plc_modbus_cfg_init() already documented for the
 * old sx_usb_tiny_t* parameter.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Mirrors nmbs_transport's values (libs/nanomodbus/nanomodbus.h:
 * NMBS_TRANSPORT_RTU = 1, NMBS_TRANSPORT_TCP = 2) without including
 * nanomodbus.h from this header. plc_modbus_cfg.c is the only place that
 * casts this back to nmbs_transport when building an nmbs_platform_conf,
 * since it already includes nanomodbus.h for other reasons.
 */
typedef enum {
    MODBUS_TRANSPORT_KIND_RTU = 1,
    MODBUS_TRANSPORT_KIND_TCP = 2,
} modbus_transport_kind_t;

typedef struct {
    /*
     * Opaque driver instance pointer, passed back unchanged as the arg
     * parameter of read/write/process below. Concrete meaning depends on
     * which factory function produced this value: sx_usb_tiny_t* for
     * modbus_transport_usb_create(), sx_uart_t* for a future
     * modbus_transport_uart_create(), and so on. Not owned by this
     * struct; see the ownership note above.
     */
    void *ctx;

    /*
     * Same contract as nmbs_platform_conf.read (nanomodbus.h): return
     * the number of bytes actually read (0 <= n <= count), or a negative
     * value on a hard transport error. A return value less than count,
     * including 0, is treated as a timeout, not an error, and is
     * expected behavior for a non-blocking poll (timeout_ms == 0).
     */
    int32_t (*read)(uint8_t *buf, uint16_t count, int32_t timeout_ms, void *ctx);

    /*
     * Same contract as nmbs_platform_conf.write (nanomodbus.h): return
     * count on success, or a negative value on a hard transport error
     * (e.g. not connected).
     */
    int32_t (*write)(const uint8_t *buf, uint16_t count, int32_t timeout_ms, void *ctx);

    /*
     * Per-scan-cycle pump for whatever background work this transport's
     * driver needs (e.g. tud_task() for USB-CDC). Called once per
     * modbus_config_service() iteration, before polling nanoMODBUS.
     * May be NULL if the underlying driver has no such requirement (for
     * example a UART driver serviced entirely by interrupt handlers).
     */
    void (*process)(void *ctx);

    /*
     * Which nanoMODBUS transport framing to use (RTU or TCP). Set once
     * by the factory function that built this value; plc_modbus_cfg_init()
     * passes it straight through to nmbs_platform_conf.transport instead
     * of hard-coding NMBS_TRANSPORT_RTU as it used to.
     */
    modbus_transport_kind_t kind;

    /*
     * RTU unit ID (slave address) this server answers to. Set by the
     * factory function that built this value.
     *
     * MUST be in 1..247 when kind == MODBUS_TRANSPORT_KIND_RTU:
     * nanoMODBUS's nmbs_server_create() rejects 0 outright (0 is the RTU
     * broadcast address, which never gets a reply) and returns
     * NMBS_ERROR_INVALID_ARGUMENT before nmbs_create() ever runs. It also
     * FILTERS on this value at runtime -- a request whose unit_id byte
     * differs is silently ignored, not answered -- so the App must send
     * this exact value (test_plc.py's --unit).
     *
     * Ignored when kind == MODBUS_TRANSPORT_KIND_TCP (nanoMODBUS does not
     * use address_rtu there).
     */
    uint8_t unit_id;
} modbus_transport_t;

#ifdef __cplusplus
}
#endif

#endif /* MODBUS_TRANSPORT_H */