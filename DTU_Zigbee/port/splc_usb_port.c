#include "splc_usb_port.h"
#include "splc_opt.h"

#include <stdint.h>
#include <stddef.h>

#include "splc_log.h"
#include "modbus_link.h"
#include "tusb.h"

#ifndef SPLC_USB_CDC_ITF
#define SPLC_USB_CDC_ITF 1
#endif
#if CFG_TUD_CDC <= SPLC_USB_CDC_ITF
#error "splc_usb_port: CFG_TUD_CDC must be > SPLC_USB_CDC_ITF (tusb_config.h of the host)"
#endif

/* Slave address the link answers to on USB (RTU framing; the board sets the kind). */
#ifndef SPLC_USB_UNIT_ID
#define SPLC_USB_UNIT_ID 1
#endif

/* 1 = output() fails (hard error) while the PC has not opened the port (DTR low). 0 (default) =
 * only "USB not mounted" is an error, because not every serial program raises DTR. */
#ifndef SPLC_USB_REQUIRE_DTR
#define SPLC_USB_REQUIRE_DTR 0
#endif

static const char *TAG = "USB_PORT";

static bool usb_up(void)
{
    if (!tud_inited() || !tud_mounted()) {
        return false;
    }
#if SPLC_USB_REQUIRE_DTR
    return tud_cdc_n_connected(SPLC_USB_CDC_ITF);
#else
    return true;
#endif
}

/* Core -> peer. Runs in the engine task; never blocks (contract of splc_link_ops_t.output). */
static int32_t usb_output(const uint8_t *buf, uint16_t len, void *ctx)
{
    (void)ctx;
    if (!usb_up()) {
        return -1;
    }
    const uint32_t room = tud_cdc_n_write_available(SPLC_USB_CDC_ITF);
    if (room == 0U) {
        (void)tud_cdc_n_write_flush(SPLC_USB_CDC_ITF);
        return 0;                                  /* busy: the core retries */
    }
    const uint32_t n  = (len < room) ? len : room;
    const uint32_t wr = tud_cdc_n_write(SPLC_USB_CDC_ITF, buf, n);
    (void)tud_cdc_n_write_flush(SPLC_USB_CDC_ITF);
    return (int32_t)wr;
}

/* Peer -> core. Called by the core once per scan and while read()/write() wait, i.e. ONLY from
 * the engine task: that is the single context that calls splc_link_input(). */
static void usb_process(void *ctx)
{
    (void)ctx;
    if (!tud_inited()) {
        return;
    }
    uint8_t tmp[64];
    while (tud_cdc_n_available(SPLC_USB_CDC_ITF) > 0U) {
        const uint32_t n = tud_cdc_n_read(SPLC_USB_CDC_ITF, tmp, sizeof tmp);
        if (n == 0U) {
            break;
        }
        const uint16_t taken = splc_link_input(tmp, (uint16_t)n);
        if (taken < n) {
            break;      /* RX ring full: the rest was counted in splc_link_rx_dropped() */
        }
    }
}

bool splc_usb_port_init(void)
{
    splc_link_ops_t ops = {
        .ctx     = NULL,
        .output  = usb_output,
        .process = usb_process,
        .unit_id = SPLC_USB_UNIT_ID,
    };
    const splc_link_err_t e = splc_link_register(&ops);
    if (e != SPLC_LINK_OK) {
        log_error(TAG, "splc_link_register failed (%d)", (int)e);
        return false;
    }
    log_info(TAG, "App link on USB-CDC instance %d", (int)SPLC_USB_CDC_ITF);
    return true;
}