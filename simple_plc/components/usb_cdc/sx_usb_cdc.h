#ifndef __SX_USB_CDC_H__
#define __SX_USB_CDC_H__

#include <stdint.h>
#include "cqueue.h"
#include "tusb_types.h"

typedef struct sx_usb_tiny_config {
    uint32_t rx_buf_size;
    uint32_t tx_buf_size;
} sx_usb_tiny_config_t;

typedef struct sx_usb_tiny {
    sx_usb_tiny_config_t *config;

    uint8_t  *rxBuffer;
    CQueue_t  rxQueue;

    uint8_t  *txBuffer;
    CQueue_t  txQueue;

    bool      connected;
} sx_usb_tiny_t;

#endif