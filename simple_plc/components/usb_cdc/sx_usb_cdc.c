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
static void usb_rx_task(sx_usb_tiny_t *_usb)
{
#if STM32H5_PLATFORM
    if(!tud_cdc_connected()) return;
    if(!tud_cdc_available()) return;

    uint8_t buf[64];
    log_debug(TAG, "connected=%d available=%d", tud_cdc_connected(), tud_cdc_available());
    uint32_t count = tud_cdc_read(buf, sizeof(buf));

    for(uint32_t i=0; i<count; i++){
        if (cqueue_send(&_usb->rxQueue, &buf[i]) == false) {
            log_warn(TAG, "RX queue full, byte dropped");
            break;
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
    log_debug(TAG, "USB write: %lu bytes", _len);

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
    while(len < _len && time < _timeoutMS){
        if(cqueue_receive(&_usb->rxQueue, _data + len)){
            len++;
        } else {
#if STM32H5_PLATFORM
            tud_task();
#endif
            time++;
        }
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