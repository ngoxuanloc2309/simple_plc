#ifndef MODBUS_SERIAL_H
#define MODBUS_SERIAL_H

/*
 * modbus_serial.h - Layer 3.5 (Protocol / Library Porting)
 *
 * Non-blocking Modbus RTU MASTER over a byte port supplied by the caller
 * (RS485), for the Gateway. The port is a small callback struct, so this
 * layer depends on NO UART/GPIO driver: SimplePLC's own sx_uart_*, or a host
 * firmware's RS485 driver, are plugged in by the board / port file.
 * Supports FC03 (read holding registers) and FC16 (write multiple
 * registers) for the Remote I/O link. With SPLC_MBM_ENABLE = 1 it also reads
 * coils / discrete inputs / input registers (FC01, FC02, FC04) for the Modbus
 * master blocks, up to 125 values per request.
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
 * (FC03/FC04: 5 + 2*qty bytes, FC01/FC02: 5 + ceil(qty/8) bytes, FC16: 8
 * bytes, exception: 5 bytes), so no inter-character timer is needed.
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

#include "splc_opt.h"   /* SPLC_MBM_ENABLE: every translation unit must agree on the master struct size */

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
#if SPLC_MBM_ENABLE
    /*
     * OPTIONAL non-blocking transmit (placed last so existing initialisers keep
     * working). When write_start AND tx_done are both set the master uses them
     * INSTEAD of write() (which may then be NULL): a request costs the scan only the
     * time to start the transfer, however long the frame is on the wire.
     *   write_start(ctx, buf, len): start sending, return 0 at once (negative =
     *       refused). `buf` stays valid until tx_done() returns true.
     *   tx_done(ctx): true once the LAST BIT has left the UART; polled once per scan.
     *   write_abort(ctx): optional; stops a transfer that never completes.
     * The port does the half-duplex turnaround itself FROM THE TX-COMPLETE INTERRUPT
     * and keeps the transceiver's echo out of read(). The master does NOT drain RX
     * after an asynchronous transmit (a fast slave may already be answering).
     */
    int  (*write_start)(void *ctx, const uint8_t *buf, uint16_t len);
    bool (*tx_done)(void *ctx);
    void (*write_abort)(void *ctx);
#endif
} modbus_serial_port_t;

/* Per-request limits. 125 values is the Modbus maximum for a read (frame =
 * 5 + 2*125 = 255 bytes); a write is limited to 123 registers (frame = 9 +
 * 2*123 = 255 bytes). Each master instance carries one rx buffer and one
 * value array of this size, so the large size is only used when
 * SPLC_MBM_ENABLE = 1 (the Remote I/O link needs 16 / 64). */
#ifndef MODBUS_MASTER_MAX_REGS
#if SPLC_MBM_ENABLE
#define MODBUS_MASTER_MAX_REGS   125U  /* values per request */
#else
#define MODBUS_MASTER_MAX_REGS   16U   /* per request, either direction */
#endif
#endif
#ifndef MODBUS_MASTER_FRAME_MAX
#if SPLC_MBM_ENABLE
#define MODBUS_MASTER_FRAME_MAX  256U  /* RTU ADU maximum */
#else
#define MODBUS_MASTER_FRAME_MAX  64U
#endif
#endif
#define MODBUS_MASTER_MAX_WRITE_REGS \
    ((MODBUS_MASTER_MAX_REGS) < 123U ? (MODBUS_MASTER_MAX_REGS) : 123U)

#if (MODBUS_MASTER_MAX_REGS < 1U) || (MODBUS_MASTER_MAX_REGS > 125U)
#error "MODBUS_MASTER_MAX_REGS must be 1..125"
#endif
#if MODBUS_MASTER_FRAME_MAX < (9U + 2U * MODBUS_MASTER_MAX_WRITE_REGS)
#error "MODBUS_MASTER_FRAME_MAX too small for MODBUS_MASTER_MAX_REGS"
#endif

#if SPLC_MBM_ENABLE
/* Function codes accepted by modbus_rtu_master_read_fc(). */
typedef enum {
    MODBUS_FC_READ_COILS          = 0x01,
    MODBUS_FC_READ_DISCRETE       = 0x02,
    MODBUS_FC_READ_HOLDING        = 0x03,
    MODBUS_FC_READ_INPUT          = 0x04
} modbus_read_fc_t;
#endif

typedef enum {
    MODBUS_MASTER_IDLE = 0,    /* nothing in flight, no result pending      */
    MODBUS_MASTER_BUSY,        /* request sent, waiting for the answer      */
    MODBUS_MASTER_DONE_OK,     /* answer received and valid (returned once) */
    MODBUS_MASTER_DONE_ERR     /* failed, see modbus_rtu_master_error()     */
#if SPLC_MBM_ENABLE
    , MODBUS_MASTER_SENDING    /* internal: frame going out (non-blocking TX); poll() reports BUSY */
#endif
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
#if SPLC_MBM_ENABLE
    uint32_t  resp_timeout_ms;        /* answer timeout, counted from the end of the transmit */
    uint32_t  tx_deadline_ms;         /* give up on a transmit that never completes */
    uint16_t  min_gap_ms;             /* configured inter-frame delay (3.5 chars always kept) */
#endif
    /* While a frame goes out asynchronously rx[] holds that frame; receiving starts after. */
    uint8_t   rx[MODBUS_MASTER_FRAME_MAX];
    uint16_t  rx_len;
    uint16_t  regs[MODBUS_MASTER_MAX_REGS];   /* read answer, valid after DONE_OK */
} modbus_rtu_master_t;

/* The port struct is copied; port->ctx (if any) is borrowed and must outlive
 * the master. Returns false (master unusable, every request fails) if m/port
 * is NULL or port->read / port->write is NULL. */
bool modbus_rtu_master_init(modbus_rtu_master_t *m, const modbus_serial_port_t *port);

/*
 * Starts a request. Returns false (nothing sent) if a request is already in
 * flight, if arguments are out of range (unit must be 1..247, qty
 * 1..MODBUS_MASTER_MAX_REGS for a read, 1..MODBUS_MASTER_MAX_WRITE_REGS for a
 * write), or if the inter-frame gap has not elapsed yet -- the caller just
 * tries again on a later cycle.
 *
 * modbus_rtu_master_read() is FC03. With SPLC_MBM_ENABLE = 1,
 * modbus_rtu_master_read_fc() also reads coils / discrete inputs (FC01/FC02,
 * one bit per value: regs[i] is 0 or 1) and input registers (FC04); an
 * unsupported fc is refused (returns false).
 */
bool modbus_rtu_master_read(modbus_rtu_master_t *m, uint8_t unit, uint16_t addr,
                            uint16_t qty, uint32_t timeout_ms);
#if SPLC_MBM_ENABLE
bool modbus_rtu_master_read_fc(modbus_rtu_master_t *m, uint8_t unit, uint8_t fc,
                               uint16_t addr, uint16_t qty, uint32_t timeout_ms);
#endif
bool modbus_rtu_master_write(modbus_rtu_master_t *m, uint8_t unit, uint16_t addr,
                             const uint16_t *values, uint16_t qty, uint32_t timeout_ms);
#if SPLC_MBW_ENABLE
/* MB_WRITE: write with function code FC05 (one coil: values[0] != 0 -> 0xFF00, qty
 * must be 1), FC06 (one register: values[0], qty 1), FC15 (qty coils, values[i] != 0
 * is ON) or FC16 (qty registers, same as modbus_rtu_master_write()). The answer is
 * checked against the request (address and quantity / value echo). Same rules and
 * return value as the other requests. */
bool modbus_rtu_master_write_fc(modbus_rtu_master_t *m, uint8_t unit, uint8_t fc,
                                uint16_t addr, const uint16_t *values, uint16_t qty,
                                uint32_t timeout_ms);
#endif

/*
 * Call every scan cycle while a request is in flight. Collects received
 * bytes and returns the state. DONE_OK / DONE_ERR are each returned exactly
 * once (by the call that finished the request); the master is IDLE again
 * afterwards.
 */
modbus_master_state_t modbus_rtu_master_poll(modbus_rtu_master_t *m);

#if SPLC_MBM_ENABLE
/* Minimum pause between two requests (the port's "inter-frame delay"). The 3.5
 * character gap is always kept; this can only lengthen it. 0 = just 3.5 chars. */
void modbus_rtu_master_set_min_gap_ms(modbus_rtu_master_t *m, uint16_t gap_ms);
/* true while a request is in flight (frame going out or answer awaited). */
bool modbus_rtu_master_is_busy(const modbus_rtu_master_t *m);
#endif

/* Valid after the poll() that returned DONE_OK for a read: qty values (registers,
 * or one bit each, 0/1, for FC01/FC02). */
const uint16_t *modbus_rtu_master_regs(const modbus_rtu_master_t *m);

/* Valid after the poll() that returned DONE_ERR. */
modbus_master_error_t modbus_rtu_master_error(const modbus_rtu_master_t *m);
uint8_t modbus_rtu_master_exception(const modbus_rtu_master_t *m);

#ifdef __cplusplus
}
#endif

#endif /* MODBUS_SERIAL_H */