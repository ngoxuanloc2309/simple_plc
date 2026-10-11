#ifndef SPLC_USB_CDC_H
#define SPLC_USB_CDC_H

/*
 * splc_usb_cdc.h - Layer 3.5, optional module "USB-CDC App link"
 *
 * Built only when SPLC_PORT_USB_CDC = 1 in splcopts.h (docs/architecture.md
 * section 3.1). It is the product-supplied "ethernetif.c" of the lwIP analogy,
 * shipped with the library for the one transport SimplePLC owns completely:
 * the USB-CDC App link of boards that have their own USB peripheral (Zigbee-IO).
 *
 * SimplePLC OWNS the USB peripheral here (TinyUSB, descriptors, interrupt), so a
 * firmware whose host already contains TinyUSB must NOT enable this module; it
 * leaves SPLC_PORT_USB_CDC at 0 and plugs its own pipe into port/modbus_link.
 *
 * What a board file does (see board_zigbee_io.c):
 *     board_hw_init():            splc_usb_cdc_init();
 *                                 [optional settle loop: splc_usb_cdc_process()]
 *     USB_DRD_FS_IRQHandler():    splc_usb_irq();
 *     board_get_modbus_transport(): splc_link_get_transport(&t);
 * The board file includes nothing from TinyUSB.
 *
 * Threading: everything here, except splc_usb_irq(), runs in the engine context
 * (the one that calls plc_engine_poll()). splc_usb_cdc_process() is the single
 * context that calls splc_link_input(), as modbus_link.h requires.
 */

#include <stdbool.h>
#include "splc_opt.h"
#include "modbus_link.h"

#if !SPLC_PORT_USB_CDC
#error "splc_usb_cdc.h is part of the optional USB-CDC port module: set SPLC_PORT_USB_CDC to 1 in splcopts.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Register the USB-CDC pipe with the App link (unit id SPLC_MODBUS_UNIT_ID,
 * Modbus RTU framing) and start TinyUSB in device mode (Full Speed). Call once,
 * from board_hw_init(), before plc_modbus_cfg_init() asks the link for its
 * transport. Returns SPLC_LINK_OK, or the modbus_link error (USB is then NOT
 * started, because a device whose App can never reach it helps nobody).
 */
splc_link_err_t splc_usb_cdc_init(void);

/*
 * Run the USB stack once (tud_task()) and move every byte TinyUSB has received
 * into the link (splc_link_input()). Never waits. The link calls it by itself
 * once per scan and while a read/write waits; a board calls it directly only
 * to keep USB alive outside the engine loop (e.g. the enumeration settle loop
 * in board_hw_init()).
 */
void splc_usb_cdc_process(void);

/* True while a host has the CDC port open (DTR set). */
bool splc_usb_cdc_connected(void);

/*
 * USB interrupt entry. The board's USB_DRD_FS_IRQHandler() calls exactly this,
 * so the board never needs the TinyUSB headers.
 */
void splc_usb_irq(void);

#ifdef __cplusplus
}
#endif

#endif /* SPLC_USB_CDC_H */