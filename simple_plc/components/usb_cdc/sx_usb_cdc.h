#ifndef __SX_USB_CDC_H
#define __SX_USB_CDC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "cqueue.h"
#include "tusb_types.h"
  
/*  Config  */
typedef struct sx_usb_tiny_config {
    uint32_t rx_buf_size;
    uint32_t tx_buf_size;
} sx_usb_tiny_config_t;

/*  Handle  */
typedef struct sx_usb_tiny sx_usb_tiny_t;

/*
 * rxBuffer/txBuffer are caller-owned static storage, NOT allocated by
 * sx_usb_tiny_init(). Caller must set rxBuffer/txBuffer (sized to match
 * config->rx_buf_size/tx_buf_size) BEFORE calling sx_usb_tiny_init() --
 * same ownership pattern as sx_uart_t (components/uart/sx_uart.h). No
 * malloc anywhere in this driver, per the project's no-dynamic-heap rule
 * (docs/architecture.md, Design principles).
 */
struct sx_usb_tiny {
    sx_usb_tiny_config_t *config;

    uint8_t  *rxBuffer;
    CQueue_t  rxQueue;

    uint8_t  *txBuffer;
    CQueue_t  txQueue;

    bool      connected;
};

void sx_usb_tiny_init(sx_usb_tiny_t *_usb, sx_usb_tiny_config_t *_config);
void sx_usb_tiny_process(sx_usb_tiny_t *_usb);
void sx_usb_tiny_write(sx_usb_tiny_t *_usb, const uint8_t *_data, uint32_t _len);
bool sx_usb_tiny_connected(sx_usb_tiny_t *_usb);
int sx_usb_tiny_available(sx_usb_tiny_t *_usb);
int sx_usb_tiny_read(sx_usb_tiny_t *_usb, uint8_t *_data, uint32_t _len, uint32_t _timeoutMS);
void sx_usb_tiny_printf(sx_usb_tiny_t *_usb, const char *fmt, ...);

void tud_mount_cb(void);
void tud_umount_cb(void);
void tud_suspend_cb(bool remote_wakeup_en);
void tud_resume_cb(void);

#ifdef __cplusplus
}
#endif

#endif /* __SX_USB_TINY_H */