#ifndef MODBUS_TCP_MASTER_H
#define MODBUS_TCP_MASTER_H
/*
 * modbus_tcp_master.h - Layer 3.5 (Protocol / Library Porting)
 *
 * Non-blocking Modbus TCP MASTER on libs/mb_master_slave (SPLC_MODBUS_LIB_MB only;
 * SPLC_MODBUS_TCP_MASTER_ENABLE = 1). Same shape as the RS-485 master of
 * modbus_serial.h: start a request, call poll() on later scans, read the result.
 *
 * Who owns what (lwIP style, decision 13):
 *   - the HOST owns the socket. It registers four callbacks (connect / read / write /
 *     close); SimplePLC never touches a network stack.
 *   - SimplePLC owns the MBAP framing, transaction ids, the answer timeout, the
 *     connect timeout / retry pause and the mapping of everything to the same
 *     modbus_master_state_t / modbus_master_error_t the RTU master reports.
 *
 * One master = one connection to one (host, port) = one request in flight. For several
 * slaves create several masters (SPLC_MODBUS_TCP_MASTER_COUNT). Everything runs in ONE
 * context (the engine / the caller of poll()); not from an ISR.
 *
 * Connection handling (automatic): a request on a master that is not connected first
 * connects (the request stays BUSY, at most connect_timeout_ms), then the answer timeout
 * counts from the moment the frame is sent. A failed connect ends the request with
 * MODBUS_MASTER_ERR_CONNECT and pauses further requests for retry_ms (start returns false).
 * A connection that is lost (read/write failed, peer closed) is closed through the host
 * and re-opened by the next request. Three answer timeouts in a row also drop the
 * connection (a half-open socket never recovers); a single late answer is harmless:
 * the transaction id filters it.
 *
 * Host callbacks:
 *   connect(ctx, host, port)  called every scan until it stops saying "in progress".
 *                             0 = connected (also if it already is), > 0 = still connecting,
 *                             < 0 = failed (the host cleans up). Must not block for long.
 *                             `host` is the string given to set_target() (IP literal or a
 *                             name the host can resolve).
 *   read(ctx, buf, max)       non-blocking; bytes copied (0 = nothing yet), < 0 = lost.
 *   write(ctx, buf, len)      send the whole frame; return the bytes taken (!= len = lost).
 *   close(ctx)                close the socket (idempotent).
 */
#include <stdbool.h>
#include <stdint.h>
#include "modbus_serial.h"   /* modbus_master_state_t / _error_t, MODBUS_MASTER_MAX_REGS */

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SPLC_MODBUS_TCP_MASTER_HOST_MAX
#define SPLC_MODBUS_TCP_MASTER_HOST_MAX 40
#endif

typedef struct {
    int  (*connect)(void *ctx, const char *host, uint16_t port);
    int  (*read)(void *ctx, uint8_t *buf, uint16_t max_len);
    int  (*write)(void *ctx, const uint8_t *buf, uint16_t len);
    void (*close)(void *ctx);
    void *ctx;                 /* passed back to every callback */
} modbus_tcp_master_port_t;

typedef struct {
    modbus_tcp_master_port_t port;      /* copied by modbus_tcp_master_init() */
    /* --- private state --- */
    char      host[SPLC_MODBUS_TCP_MASTER_HOST_MAX];
    uint16_t  tcp_port;                 /* 0 = target not set */
    modbus_master_state_t state;
    modbus_master_error_t error;
    uint8_t   exception_code;
    uint8_t   unit;
    uint8_t   func;
    uint8_t   conn;                     /* connection state */
    uint8_t   phase;                    /* request phase: connecting / exchanging */
    uint8_t   mb_qstate;                /* eMBMQueryState of the request in flight */
    uint8_t   timeouts;                 /* consecutive answer timeouts */
    uint16_t  addr;
    uint16_t  qty;
    uint32_t  timeout_ms;
    uint32_t  connect_ms;
    uint32_t  retry_ms;
    uint32_t  conn_deadline_ms;
    uint32_t  quiet_until_ms;           /* earliest next request */
    void     *mb_hdl;                   /* xMBMHandle, NULL = no library instance yet */
    int       mb_ch;                    /* mb_port channel, -1 = none */
    uint8_t   mb_bits[32];              /* packed bits: FC01/02 answer, FC15 request (<= 256 coils) */
    uint16_t  regs[MODBUS_MASTER_MAX_REGS];   /* read answer / write values */
} modbus_tcp_master_t;

/* port is copied; port->ctx is borrowed and must outlive the master. Returns false (master
 * unusable, every request refused) for NULL, a missing callback, or no free channel. A second
 * init of the same object releases its old channel (and connection) first. */
bool modbus_tcp_master_init(modbus_tcp_master_t *m, const modbus_tcp_master_port_t *port);
void modbus_tcp_master_deinit(modbus_tcp_master_t *m);

/* `host` is copied (< SPLC_MODBUS_TCP_MASTER_HOST_MAX), tcp_port 1..65535. false (nothing
 * changed) for bad arguments or while a request is in flight. A changed target drops the connection. */
bool modbus_tcp_master_set_target(modbus_tcp_master_t *m, const char *host, uint16_t tcp_port);
void modbus_tcp_master_set_connect_timeout_ms(modbus_tcp_master_t *m, uint32_t ms);   /* 100..60000 */
void modbus_tcp_master_set_retry_ms(modbus_tcp_master_t *m, uint32_t ms);             /* 0..60000 */

/* Start a request. false (nothing happened) if one is in flight, no target, bad arguments
 * (unit 1..247 or 255; qty 1..MODBUS_MASTER_MAX_REGS read, 1..MODBUS_MASTER_MAX_WRITE_REGS FC16,
 * FC15 up to 256 coils) or the pause after a failed connect has not elapsed.
 *   read_fc : FC01/02/03/04 (bits come back one per regs[i], 0/1)
 *   write_fc: FC05 (qty 1, values[0] != 0 -> ON) / FC06 (qty 1) / FC15 / FC16 */
bool modbus_tcp_master_read(modbus_tcp_master_t *m, uint8_t unit, uint16_t addr, uint16_t qty, uint32_t timeout_ms);
bool modbus_tcp_master_read_fc(modbus_tcp_master_t *m, uint8_t unit, uint8_t fc, uint16_t addr, uint16_t qty, uint32_t timeout_ms);
bool modbus_tcp_master_write(modbus_tcp_master_t *m, uint8_t unit, uint16_t addr, const uint16_t *values, uint16_t qty, uint32_t timeout_ms);
bool modbus_tcp_master_write_fc(modbus_tcp_master_t *m, uint8_t unit, uint8_t fc, uint16_t addr, const uint16_t *values, uint16_t qty, uint32_t timeout_ms);

/* Call every scan. BUSY while a request runs; DONE_OK / DONE_ERR are each returned exactly once
 * (IDLE afterwards). While idle with a connection up it notices a peer that closed and discards
 * stray bytes. */
modbus_master_state_t modbus_tcp_master_poll(modbus_tcp_master_t *m);

const uint16_t       *modbus_tcp_master_regs(const modbus_tcp_master_t *m);      /* after DONE_OK of a read */
modbus_master_error_t modbus_tcp_master_error(const modbus_tcp_master_t *m);     /* after DONE_ERR; CONNECT/TIMEOUT/EXCEPTION/FRAME */
uint8_t               modbus_tcp_master_exception(const modbus_tcp_master_t *m);
bool modbus_tcp_master_is_busy(const modbus_tcp_master_t *m);
bool modbus_tcp_master_is_connected(const modbus_tcp_master_t *m);
void modbus_tcp_master_disconnect(modbus_tcp_master_t *m);   /* ignored while busy */

#ifdef __cplusplus
}
#endif
#endif /* MODBUS_TCP_MASTER_H */