#include "sx_usb_cdc.h"
#include "sx_platform_config.h"
#include "logger.h"
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
static const char *TAG = "SX_USB_TINY";

#if STM32H5_PLATFORM
    #include "stm32h5xx_hal.h"
    #include "tusb.h"
    #include "usb.h"
#endif

void sx_usb_tiny_init(sx_usb_tiny_t *_usb, sx_usb_tiny_config_t *_config)
{
    _usb->config    = _config;
    _usb->connected = false;

    /* rxBuffer/txBuffer must already be set by the caller to static
     * storage sized to match rx_buf_size/tx_buf_size -- no malloc here,
     * per the project's no-dynamic-heap rule. Same contract as
     * sx_uart_t (components/uart/sx_uart.h). */
    cqueue_init_static(&_usb->rxQueue, _usb->rxBuffer,
                        _config->rx_buf_size, 1);
    cqueue_init_static(&_usb->txQueue, _usb->txBuffer,
                        _config->tx_buf_size, 1);

    /* No mutex here -- this project is bare-metal single-threaded
     * (no RTOS), unlike the WS_v1 reference this was ported from. */

#if STM32H5_PLATFORM
    //dcd_fs_msp_init(0);

    tusb_rhport_init_t dev_init = {
        .role  = TUSB_ROLE_DEVICE,
        /* USB_DRD_FS (stm32_fsdev) is Full-Speed only on STM32H5 --
         * TUSB_SPEED_AUTO left this ambiguous. Matches the known-working
         * USB_ETH reference project (board.c's usb_device_task()), which
         * sets TUSB_SPEED_FULL explicitly. */
        .speed = TUSB_SPEED_FULL,
    };
    tusb_init(BOARD_TUD_RHPORT, &dev_init);
#endif

    log_info(TAG, "CDC initialized, rx_buf=%lu tx_buf=%lu",
             _config->rx_buf_size, _config->tx_buf_size);
}

/*RX_TASK*/
/*
 * Drains the ENTIRE TinyUSB CDC RX FIFO into rxQueue, not just one
 * CFG_TUD_CDC_RX_BUFSIZE-sized (64 byte) chunk.
 *
 * Why this loop is required: a single Modbus RTU frame can be longer
 * than 64 bytes (e.g. FC16 writing 2+ SPLC_RuleRecord's worth of
 * STAGING_RULE_TABLE registers -- 2 rules = 73 bytes on the wire), so it
 * arrives from the host as more than one USB Full-Speed packet. The
 * previous version called tud_cdc_read() exactly once per
 * sx_usb_tiny_process() call (once per 10 ms scan cycle), so only the
 * first ~64 bytes of a longer frame made it into rxQueue on this scan
 * cycle; the rest was still sitting in TinyUSB's own FIFO, not lost, but
 * not visible to rxQueue yet either.
 *
 * That alone would just be a 10 ms delay -- harmless -- except
 * nmbs_server_poll() (plc_modbus_cfg.c, byte_timeout_ms == 0, "never
 * blocks the scan budget" by design) calls msg_state_reset() every time
 * it runs and treats "fewer bytes available than the frame needs right
 * now" as NMBS_ERROR_TIMEOUT for that whole request, discarding
 * everything read so far. The remaining bytes of the same frame then
 * turn up in rxQueue on the NEXT scan cycle, get parsed as if they were
 * the START of a brand new frame, and desync nanoMODBUS's framing until
 * something coincidentally realigns (in practice: the connection just
 * stops responding, matching the "works with 1 rule (41 B, fits in one
 * 64 B packet), times out with 2+ rules (73+ B)" symptom).
 *
 * Looping here until tud_cdc_available() reports empty -- not adding a
 * blocking wait -- keeps modbus_config_service()'s "never blocks"
 * contract intact: this only ever drains bytes TinyUSB's ISR has
 * ALREADY placed in its FIFO by the time sx_usb_tiny_process() runs; it
 * never waits for bytes that have not arrived yet. Multiple back-to-back
 * USB packets that already landed before this call are simply no longer
 * left behind one at a time.
 */
static void usb_rx_task(sx_usb_tiny_t *_usb)
{
#if STM32H5_PLATFORM
    if(!tud_cdc_connected()) return;

    uint8_t buf[64];
    while (tud_cdc_available()) {
        uint32_t count = tud_cdc_read(buf, sizeof(buf));
        if (count == 0) {
            break; /* Defensive: available() said >0 but read() got nothing this call. */
        }

        // log_debug(TAG, "connected=%d available=%d read=%lu",
        //           tud_cdc_connected(), tud_cdc_available(), (unsigned long)count);

        for(uint32_t i=0; i<count; i++){
            if (cqueue_send(&_usb->rxQueue, &buf[i]) == false) {
                log_warn(TAG, "RX queue full, byte dropped");
                return; /* rxQueue itself is full -- draining more would just drop more. */
            }
        }
    }
#endif
}

void sx_usb_tiny_process(sx_usb_tiny_t *_usb){
#if STM32H5_PLATFORM
    tud_task();
    _usb->connected = sx_usb_tiny_connected(_usb);
    usb_rx_task(_usb);
#endif
}

void sx_usb_tiny_write(sx_usb_tiny_t *_usb, const uint8_t *_data, uint32_t _len){
    if(!sx_usb_tiny_connected(_usb)) 
        return;
    // log_debug(TAG, "USB write: %lu bytes", _len);

#if STM32H5_PLATFORM
    uint32_t sent = 0;
    while(sent < _len){
        uint32_t written = tud_cdc_write(_data + sent, _len - sent);
        sent += written;
        tud_cdc_write_flush();
        if(written == 0)
            tud_task();
    }
#endif
}

int sx_usb_tiny_read(sx_usb_tiny_t *_usb, uint8_t *_data,
                     uint32_t _len, uint32_t _timeoutMS)
{
    uint32_t len  = 0;
    uint32_t time = 0;

    /*
     * Bytes ALREADY sitting in rxQueue are always returned, regardless of
     * _timeoutMS. The timeout only bounds how long we WAIT for bytes that
     * have not arrived yet.
     *
     * The previous loop was `while (len < _len && time < _timeoutMS)`, so
     * with _timeoutMS == 0 the body never ran and this returned 0 even when
     * rxQueue held a complete frame. plc_modbus_cfg sets nanoMODBUS's read
     * and byte timeouts to 0 (non-blocking poll), so every recv() got 0
     * bytes back -> NMBS_ERROR_TIMEOUT -> nmbs_server_poll() returned
     * having consumed nothing, and the request sat in the FIFO forever
     * (symptom: log "available=8" repeating, App sees "No response").
     *
     * nanoMODBUS's platform.read contract: timeout 0 = "return whatever is
     * available right now, do not wait".
     */
    while (len < _len) {
        if (cqueue_receive(&_usb->rxQueue, _data + len)) {
            len++;
            continue;
        }

        /* Queue is empty. Give up if the caller does not want to wait, or
         * the wait budget is used up. */
        if (time >= _timeoutMS) {
            break;
        }

#if STM32H5_PLATFORM
        tud_task();
#endif
        time++;
    }
    return (int)len;
}

bool sx_usb_tiny_connected(sx_usb_tiny_t *_usb){
    (void)_usb;
#if STM32H5_PLATFORM
    return tud_cdc_connected();
#else
    return false;
#endif
}

int sx_usb_tiny_available(sx_usb_tiny_t *_usb){
    //log_debug(TAG, "USB available: %d bytes", _usb->rxQueue.count);
    return _usb->rxQueue.count;
    //return tud_cdc_available();
}

void sx_usb_tiny_printf(sx_usb_tiny_t *_usb, const char *fmt, ...){
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    sx_usb_tiny_write(_usb, (uint8_t *)buf, strlen(buf));
}

void tud_mount_cb(void)
{
  log_debug(TAG, "USB Mounted!\r\n");
}

void tud_umount_cb(void)
{
  log_debug(TAG, "USB Unmounted!\r\n");
}

void tud_suspend_cb(bool remote_wakeup_en)
{
  (void)remote_wakeup_en;
  log_debug(TAG, "USB Suspended\r\n");
}

void tud_resume_cb(void)
{
    log_debug(TAG, "USB Resumed\r\n");
}