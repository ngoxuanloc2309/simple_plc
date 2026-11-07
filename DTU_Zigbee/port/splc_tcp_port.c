#include "splc_tcp_port.h"
#include "splc_opt.h"

#include <stddef.h>
#include <string.h>

#include "splc_log.h"
#include "modbus_transport.h"

#if SPLC_MODBUS_MAX_TRANSPORTS > 1

#include "plc_modbus_cfg.h"
#include "sx_time.h"
#include "lwip/sockets.h"

#ifndef SPLC_TCP_PORT_NUMBER
#define SPLC_TCP_PORT_NUMBER 502
#endif
#ifndef SPLC_TCP_WRITE_TIMEOUT_MS
#define SPLC_TCP_WRITE_TIMEOUT_MS 1000
#endif
/* Retry period of a failed socket()/bind()/listen(): a down network must not be hammered. */
#define LISTEN_RETRY_MS 1000U

static const char *TAG = "TCP_PORT";

static int s_listen = -1;
static int s_client = -1;
static uint32_t s_listen_try_ms;
static bool     s_listen_tried;
static splc_tcp_port_filter_fn s_filter;

void splc_tcp_port_set_filter(splc_tcp_port_filter_fn fn)
{
    s_filter = fn;
}

static bool would_block(int e)
{
    return (e == EWOULDBLOCK) || (e == EAGAIN);
}

static void client_close(void)
{
    if (s_client >= 0) {
        (void)lwip_close(s_client);
        s_client = -1;
        log_info(TAG, "client closed");
    }
}

static void listen_open(void)
{
    const uint32_t now = sx_get_tick_ms();
    if (s_listen_tried && (uint32_t)(now - s_listen_try_ms) < LISTEN_RETRY_MS) {
        return;
    }
    s_listen_tried  = true;
    s_listen_try_ms = now;

    const int fd = lwip_socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        log_error(TAG, "socket() failed (errno %d)", errno);
        return;
    }
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family      = AF_INET;
    a.sin_port        = lwip_htons((uint16_t)SPLC_TCP_PORT_NUMBER);
    a.sin_addr.s_addr = lwip_htonl(INADDR_ANY);
    const int one = 1;
    (void)lwip_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (lwip_bind(fd, (struct sockaddr *)&a, sizeof a) < 0 ||
        lwip_listen(fd, 1) < 0 ||
        lwip_fcntl(fd, F_SETFL, lwip_fcntl(fd, F_GETFL, 0) | O_NONBLOCK) < 0) {
        log_error(TAG, "bind/listen failed (errno %d)", errno);
        (void)lwip_close(fd);
        return;
    }
    s_listen = fd;
    log_info(TAG, "Modbus TCP listening on port %d", (int)SPLC_TCP_PORT_NUMBER);
}

/* Non-blocking accept; a new peer replaces the current client. */
static void accept_poll(void)
{
    struct sockaddr_in peer;
    socklen_t plen = sizeof peer;
    const int fd = lwip_accept(s_listen, (struct sockaddr *)&peer, &plen);
    if (fd < 0) {
        if (!would_block(errno)) {
            log_error(TAG, "accept() failed (errno %d): reopening", errno);
            (void)lwip_close(s_listen);
            s_listen = -1;
        }
        return;
    }
    if (s_filter != NULL && !s_filter((uint32_t)peer.sin_addr.s_addr)) {
        log_warn(TAG, "peer refused by filter");
        (void)lwip_close(fd);
        return;
    }
    if (lwip_fcntl(fd, F_SETFL, lwip_fcntl(fd, F_GETFL, 0) | O_NONBLOCK) < 0) {
        (void)lwip_close(fd);
        return;
    }
    const int one = 1;
    (void)lwip_setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    client_close();                     /* one App at a time: the newest wins */
    s_client = fd;
    log_info(TAG, "client connected");
}

/* Wait until fd is readable (rd) or writable, at most timeout_ms. */
static bool wait_fd(int fd, bool rd, int32_t timeout_ms)
{
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
    const int r = rd ? lwip_select(fd + 1, &set, NULL, NULL, &tv)
                     : lwip_select(fd + 1, NULL, &set, NULL, &tv);
    return r > 0;
}

/* modbus_transport_t.read: 0 while no client; never blocks when timeout_ms == 0. */
static int32_t tcp_read(uint8_t *buf, uint16_t count, int32_t timeout_ms, void *ctx)
{
    (void)ctx;
    if (s_listen < 0) {
        listen_open();
    }
    if (s_listen >= 0) {
        accept_poll();
    }
    if (s_client < 0) {
        return 0;
    }
    if (timeout_ms > 0 && !wait_fd(s_client, true, timeout_ms)) {
        return 0;
    }
    const int n = lwip_recv(s_client, buf, count, MSG_DONTWAIT);
    if (n > 0) {
        return n;
    }
    if (n < 0 && would_block(errno)) {
        return 0;
    }
    client_close();                     /* n == 0: orderly close; n < 0: error */
    return 0;
}

/* modbus_transport_t.write: all `count` bytes or a negative value. */
static int32_t tcp_write(const uint8_t *buf, uint16_t count, int32_t timeout_ms, void *ctx)
{
    (void)ctx;
    if (s_client < 0) {
        return -1;
    }
    const int32_t budget = (timeout_ms > 0) ? timeout_ms : SPLC_TCP_WRITE_TIMEOUT_MS;
    const uint32_t t0 = sx_get_tick_ms();
    uint16_t sent = 0U;
    while (sent < count) {
        const int n = lwip_send(s_client, buf + sent, (size_t)(count - sent), MSG_DONTWAIT);
        if (n > 0) {
            sent = (uint16_t)(sent + (uint16_t)n);
            continue;
        }
        if (n < 0 && !would_block(errno)) {
            client_close();
            return -1;
        }
        const uint32_t used = (uint32_t)(sx_get_tick_ms() - t0);
        if (used >= (uint32_t)budget || !wait_fd(s_client, false, (int32_t)((uint32_t)budget - used))) {
            log_warn(TAG, "write timeout, closing client");
            client_close();
            return -1;
        }
    }
    return (int32_t)count;
}

bool splc_tcp_port_attach(void)
{
    modbus_transport_t t;
    memset(&t, 0, sizeof t);
    t.ctx     = NULL;
    t.read    = tcp_read;
    t.write   = tcp_write;
    t.process = NULL;
    t.kind    = MODBUS_TRANSPORT_KIND_TCP;
    t.unit_id = 0;                      /* ignored for TCP */
    if (!plc_modbus_cfg_add_transport(&t)) {
        log_error(TAG, "plc_modbus_cfg_add_transport refused the TCP transport");
        return false;
    }
    return true;
}

#else /* SPLC_MODBUS_MAX_TRANSPORTS < 2 */

void splc_tcp_port_set_filter(splc_tcp_port_filter_fn fn) { (void)fn; }

bool splc_tcp_port_attach(void)
{
    log_error("TCP_PORT", "SPLC_MODBUS_MAX_TRANSPORTS is 1: no room for a TCP transport");
    return false;
}

#endif