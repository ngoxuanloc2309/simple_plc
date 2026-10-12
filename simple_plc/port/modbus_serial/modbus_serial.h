#ifndef MODBUS_SERIAL_H
#define MODBUS_SERIAL_H

/*
 * modbus_serial.h - Layer 3.5 (Protocol / Library Porting)
 *
 * Non-blocking Modbus RTU MASTER over a byte port supplied by the caller
 * (RS485), for the Gateway. The port is a small callback struct, so this
 * layer depends on NO UART/GPIO driver: SimplePLC's own sx_uart_*, or a host
 * firmware's RS485 driver, are plugged in by the board / port file.
 * Supports exactly what the Remote I/O link needs: FC03 (read holding
 * registers) and FC16 (write multiple registers).
 *
 * Why not nanoMODBUS's client: its calls block until the slave answers
 * (up to the whole timeout), which would stall the scan cycle. Here a
 * request is started (the TX itself is a short blocking write, ~1.5 ms for
 * 17 bytes at 115200 baud) and the answer is collected by
 * modbus_rtu_master_poll() on later scan cycles, using real ticks for the
 * timeout.
 *
 * One request in flight at a time per master (one RS485 bus = one master).
 * A frame is complete when its length is known from the function code
 * (FC03: 5 + 2*qty bytes, FC16: 8 bytes, exception: 5 bytes), so no
 * inter-character timer is needed.
 *
 * RS485 direction: if the port provides set_tx() the master calls it with
 *  true right before write() and false right after (the write callback must
 *  return only after the last bit has left the UART). If set_tx is NULL the
 *  port handles direction itself (e.g. a host RS485 driver). Bytes echoed back
 *  by a transceiver that keeps its receiver on are discarded after the
 *  transmit.
 *
 * Contract: the port must already be initialised and its baudrate field must
 *  equal the real baud rate (it only feeds the inter-frame gap calculation).
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Byte port the master talks through. All callbacks run in the caller's
 * context (the engine task); none may block longer than stated.
 */
typedef struct {
    /* Non-blocking: copy up to max_len received bytes into buf and return how
     * many (0 = nothing yet). A negative value is treated as 0. REQUIRED. */
    int  (*read)(void *ctx, uint8_t *buf, uint16_t max_len);
    /* Send len bytes and return 0 once the LAST BIT has left the UART, or a
     * negative value on failure. May block for the TX time of one frame
     * (~1.5 ms for 17 bytes at 115200) up to timeout_ms. REQUIRED. */
    int  (*write)(void *ctx, const uint8_t *buf, uint16_t len, uint32_t timeout_ms);
    /* Optional RS485 direction control: true = driver enable (transmit),
     * false = receive. NULL when the port does it inside write(). */
    void (*set_tx)(void *ctx, bool tx);
    void     *ctx;        /* passed back to every callback */
    uint32_t  baudrate;   /* real line speed, for the inter-frame gap */
} modbus_serial_port_t;

#define MODBUS_MASTER_MAX_REGS   16U   /* per request, either direction */
#define MODBUS_MASTER_FRAME_MAX  64U

typedef enum {
    MODBUS_MASTER_IDLE = 0,    /* nothing in flight, no result pending      */
    MODBUS_MASTER_BUSY,        /* request sent, waiting for the answer      */
    MODBUS_MASTER_DONE_OK,     /* answer received and valid (returned once) */
    MODBUS_MASTER_DONE_ERR     /* failed, see modbus_rtu_master_error()     */
} modbus_master_state_t;

typedef enum {
    MODBUS_MASTER_ERR_NONE = 0,
    MODBUS_MASTER_ERR_TIMEOUT,     /* no complete answer in time             */
    MODBUS_MASTER_ERR_CRC,         /* CRC mismatch                           */
    MODBUS_MASTER_ERR_FRAME,       /* wrong unit/function/length/echo        */
    MODBUS_MASTER_ERR_EXCEPTION,   /* slave answered with an exception       */
    MODBUS_MASTER_ERR_TX           /* UART transmit failed                   */
} modbus_master_error_t;

typedef struct {
    modbus_serial_port_t port;        /* copied by modbus_rtu_master_init() */

    /* --- private state --- */
    modbus_master_state_t state;
    modbus_master_error_t error;
    uint8_t   exception_code;
    uint8_t   unit;
    uint8_t   func;
    uint16_t  addr;
    uint16_t  qty;
    uint32_t  deadline_ms;
    uint32_t  quiet_until_ms;         /* earliest next request (inter-frame gap) */
    uint8_t   rx[MODBUS_MASTER_FRAME_MAX];
    uint16_t  rx_len;
    uint16_t  regs[MODBUS_MASTER_MAX_REGS];   /* FC03 answer, valid after DONE_OK */
} modbus_rtu_master_t;

/* The port struct is copied; port->ctx (if any) is borrowed and must outlive
 * the master. Returns false (master unusable, every request fails) if m/port
 * is NULL or port->read / port->write is NULL. */
bool modbus_rtu_master_init(modbus_rtu_master_t *m, const modbus_serial_port_t *port);

/*
 * Starts a request. Returns false (nothing sent) if a request is already in
 * flight, if arguments are out of range (unit must be 1..247, qty
 * 1..MODBUS_MASTER_MAX_REGS), or if the inter-frame gap has not elapsed yet
 * -- the caller just tries again on a later cycle.
 */
bool modbus_rtu_master_read(modbus_rtu_master_t *m, uint8_t unit, uint16_t addr,
                            uint16_t qty, uint32_t timeout_ms);
bool modbus_rtu_master_write(modbus_rtu_master_t *m, uint8_t unit, uint16_t addr,
                             const uint16_t *values, uint16_t qty, uint32_t timeout_ms);

/*
 * Call every scan cycle while a request is in flight. Collects received
 * bytes and returns the state. DONE_OK / DONE_ERR are each returned exactly
 * once (by the call that finished the request); the master is IDLE again
 * afterwards.
 */
modbus_master_state_t modbus_rtu_master_poll(modbus_rtu_master_t *m);

/* Valid after the poll() that returned DONE_OK for an FC03 read. */
const uint16_t *modbus_rtu_master_regs(const modbus_rtu_master_t *m);

/* Valid after the poll() that returned DONE_ERR. */
modbus_master_error_t modbus_rtu_master_error(const modbus_rtu_master_t *m);
uint8_t modbus_rtu_master_exception(const modbus_rtu_master_t *m);

#ifdef __cplusplus
}
#endif

#endif /* MODBUS_SERIAL_H */