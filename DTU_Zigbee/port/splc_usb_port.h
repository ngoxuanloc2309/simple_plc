#ifndef SPLC_USB_PORT_H
#define SPLC_USB_PORT_H
/*
 * splc_usb_port.h - product port (outside the simple_plc library)
 *
 * App link over the host's TinyUSB: CDC instance SPLC_USB_CDC_ITF (1) is a plain byte pipe for
 * splc_link_register(). The host owns the USB peripheral and runs tud_task(); this port only calls
 * tud_cdc_n_*() on that instance (instance 0 stays the host shell).
 *
 * Needs the second CDC-ACM in the host's usb_descriptors.c (USB_CDC_SPLC 1) and CFG_TUD_CDC >= 2.
 */
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Register the link with the library. Call before plc_engine_init(), from the engine task.
 * Returns false (and logs) if the library refused it. */
bool splc_usb_port_init(void);

#ifdef __cplusplus
}
#endif
#endif /* SPLC_USB_PORT_H */