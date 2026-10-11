#include "splc_usb_cdc.h"

#include <stdint.h>

#include "tusb.h"          /* only this module may include TinyUSB */
#include "splc_log.h"

/*
 * See splc_usb_cdc.h for the contract.
 *
 * This file is the whole USB side of the App link. The core (modbus_link) owns
 * the RX ring and EVERY timeout; this file only moves bytes, in two directions:
 *
 *   host -> device : splc_usb_cdc_process() drains TinyUSB's CDC FIFO into
 *                    splc_link_input(). It waits for nothing. A Modbus frame
 *                    longer than one 64-byte Full-Speed packet (e.g. an FC16 of
 *                    73 bytes) arrives as several packets; modbus_link read()
 *                    waits for the later pieces, calling process() meanwhile.
 *   device -> host : usb_output() hands bytes to TinyUSB and returns how many it
 *                    accepted (its TX FIFO is one 64-byte packet, so a long
 *                    reply is accepted in pieces). modbus_link write() retries
 *                    the rest, pumping process() = tud_task() in between.
 */

static const char *TAG = "USB_CDC";

static bool s_started;

/* --- host -> device ---------------------------------------------------- */

/* Same size as TinyUSB's CDC RX FIFO / one Full-Speed packet. */
#define USB_RX_CHUNK   64U

void splc_usb_cdc_process(void)
{
    if (!s_started) {
        return;
    }

    tud_task();

    if (!tud_cdc_connected()) {
        return;
    }

    /*
     * Drain the ENTIRE TinyUSB FIFO, not one chunk per call. A frame spanning
     * several packets must reach the link as soon as it is in the FIFO; leaving
     * the tail behind makes nanoMODBUS misread it later as a new request (the
     * "works with 1 rule, dies with 2+" bug of the old path, handoff section 5).
     * This only moves bytes that ARE already in the FIFO; it never waits.
     */
    uint8_t buf[USB_RX_CHUNK];
    while (tud_cdc_available() != 0U) {
        uint32_t count = tud_cdc_read(buf, sizeof(buf));
        if (count == 0U) {
            break;                 /* available() said > 0 but read() got nothing */
        }
        uint16_t taken = splc_link_input(buf, (uint16_t)count);
        if (taken < count) {
            /* The link's RX ring is full (it counts the dropped bytes in
             * splc_link_rx_dropped()). Stop draining: what is left stays in
             * TinyUSB's FIFO, which pushes back on the host. */
            log_warn(TAG, "link RX ring full, %u byte(s) dropped",
                     (unsigned)(count - taken));
            break;
        }
    }
}

/* --- device -> host ---------------------------------------------------- */

/* modbus_link output() contract: accept up to len bytes WITHOUT blocking,
 * return how many (0 = busy, the core retries); < 0 = hard error. */
static int32_t usb_output(const uint8_t *buf, uint16_t len, void *ctx)
{
    (void)ctx;

    if (!tud_cdc_connected()) {
        return -1;                 /* no host: the core aborts the write */
    }

    uint32_t accepted = tud_cdc_write(buf, len);
    (void)tud_cdc_write_flush();   /* start the transfer now, not at the next tud_task() */
    return (int32_t)accepted;
}

/* modbus_link process() adapter: same pump, with the ctx argument the link passes. */
static void usb_process(void *ctx)
{
    (void)ctx;
    splc_usb_cdc_process();
}

/* --- init / queries / interrupt ---------------------------------------- */

splc_link_err_t splc_usb_cdc_init(void)
{
    const splc_link_ops_t ops = {
        .ctx     = NULL,
        .output  = usb_output,
        .process = usb_process,
        .kind    = MODBUS_TRANSPORT_KIND_RTU,
        .unit_id = (uint8_t)SPLC_MODBUS_UNIT_ID,
    };

    /* Register first: bytes that reach splc_link_input() before registration
     * are dropped, and nothing can arrive before TinyUSB runs. */
    splc_link_err_t err = splc_link_register(&ops);
    if (err != SPLC_LINK_OK) {
        log_error(TAG, "App link register failed (%d) -- USB NOT started", (int)err);
        return err;
    }

    tusb_rhport_init_t dev_init = {
        .role  = TUSB_ROLE_DEVICE,
        /* USB_DRD_FS (stm32_fsdev) is Full-Speed only on STM32H5; TUSB_SPEED_AUTO
         * left this ambiguous. */
        .speed = TUSB_SPEED_FULL,
    };
    if (!tusb_init(BOARD_TUD_RHPORT, &dev_init)) {
        log_error(TAG, "tusb_init failed -- USB not running");
        splc_link_unregister();
        return SPLC_LINK_ERR_NOT_READY;
    }

    s_started = true;
    log_info(TAG, "USB-CDC link registered and TinyUSB started (unit_id=%u)",
             (unsigned)SPLC_MODBUS_UNIT_ID);
    return SPLC_LINK_OK;
}

bool splc_usb_cdc_connected(void)
{
    return s_started && tud_cdc_connected();
}

void splc_usb_irq(void)
{
    tud_int_handler(BOARD_TUD_RHPORT);
}

/* --- TinyUSB callbacks (diagnostics only) ------------------------------- */

void tud_mount_cb(void)   { log_debug(TAG, "USB Mounted!\r\n"); }
void tud_umount_cb(void)  { log_debug(TAG, "USB Unmounted!\r\n"); }
void tud_suspend_cb(bool remote_wakeup_en)
{
    (void)remote_wakeup_en;
    log_debug(TAG, "USB Suspended\r\n");
}
void tud_resume_cb(void)  { log_debug(TAG, "USB Resumed\r\n"); }