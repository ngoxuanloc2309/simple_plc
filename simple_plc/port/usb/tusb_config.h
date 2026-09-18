/*
Author: Logan Ngo
Date:   09/03/2026
*/
#ifndef TUSB_CONFIG_H_
#define TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif


#ifndef BOARD_TUD_RHPORT
#define BOARD_TUD_RHPORT      0
#endif

#ifndef BOARD_TUD_MAX_SPEED
/* USB_DRD_FS (stm32_fsdev driver) is Full-Speed only on STM32H5 --
 * OPT_MODE_DEFAULT_SPEED left the speed negotiation ambiguous. Matches
 * the known-working USB_ETH reference project's tusb_config.h, which
 * sets this explicitly. */
#define BOARD_TUD_MAX_SPEED   OPT_MODE_FULL_SPEED
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS           OPT_OS_NONE
#endif

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG        0
#endif

#define CFG_TUD_ENABLED       1
#define CFG_TUD_MAX_SPEED     BOARD_TUD_MAX_SPEED
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN    __attribute__ ((aligned(4)))
#endif

#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE    64//64
#endif

//------------- DEVICE ------------//
#define CFG_TUSB_MCU        OPT_MCU_STM32H5
#define BOARD_TUD_RHPORT    0

//------------- CLASS -------------//
#define CFG_TUD_HID               0   /* 0 = CDC only | 1 = CDC + HID */

#ifndef CFG_TUD_MSC
#define CFG_TUD_MSC   0
#endif

#ifndef CFG_TUD_CDC
#define CFG_TUD_CDC   1  
#endif

#define CFG_TUD_MIDI              0
#define CFG_TUD_VENDOR            0

// CDC
#define CFG_TUD_CDC_RX_BUFSIZE    (TUD_OPT_HIGH_SPEED ? 512 : 64)
#define CFG_TUD_CDC_TX_BUFSIZE    (TUD_OPT_HIGH_SPEED ? 512 : 64)
#define CFG_TUD_CDC_EP_BUFSIZE    (TUD_OPT_HIGH_SPEED ? 512 : 64)

// HID – ignored when CFG_TUD_HID = 0
#define CFG_TUD_HID_EP_BUFSIZE    16

#ifdef CFG_TUD_MSC
#define CFG_TUD_MSC_EP_BUFSIZE    64//512: false //64
#endif

#ifdef __cplusplus
}
#endif

#endif /* TUSB_CONFIG_H_ */