/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include "class/net/net_device.h"
#include "tusb.h"

/* A combination of interfaces must have a unique product id, since PC will save device driver after the first plug.
 * Same VID/PID with different interface e.g MSC (first), then CDC (later) will possibly cause system error on PC.
 *
 * Auto ProductID layout's Bitmap:
 * [MSB]   DFU_RUNTIME | NET | VENDOR | MIDI | HID | MSC | CDC          [LSB]
 */
#define PID_MAP(itf, n) ((CFG_TUD_##itf) ? (1 << (n)) : 0)
#define USB_PID                                                                                           \
  (0x4000 | PID_MAP(CDC, 0) | PID_MAP(MSC, 1) | PID_MAP(HID, 2) | PID_MAP(MIDI, 3) | PID_MAP(VENDOR, 4) | \
   PID_MAP(ECM_RNDIS, 5) | PID_MAP(NCM, 5) | PID_MAP(DFU_RUNTIME, 6))

// SimplePLC App link: a second CDC-ACM port (TinyUSB CDC instance 1; instance 0 stays the shell).
// Appended AFTER the DFU interface in every configuration, so every existing interface number
// (ECM/RNDIS, ACM, DFU) is unchanged and the shell port keeps CDC instance 0 (TinyUSB numbers CDC
// instances in the order the interfaces are opened). 0 = descriptors exactly as before.
#ifndef USB_CDC_SPLC
#define USB_CDC_SPLC 1
#endif
#if USB_CDC_SPLC && (CFG_TUD_CDC < 2)
#error "USB_CDC_SPLC needs CFG_TUD_CDC >= 2 (tusb_config.h)"
#endif

// String Descriptor Index
enum {
  STRID_LANGID = 0,
  STRID_MANUFACTURER,
  STRID_PRODUCT,
  STRID_SERIAL,
  STRID_INTERFACE,
  STRID_MAC,
  STRID_CDC_ACM,      // Added for ACM Port Name
  STRID_DFU_RT,        // Added for DFU Runtime interface
  STRID_CDC_SPLC,      // SimplePLC App link port name
  STRID_COUNT
};

enum {
  ITF_NUM_CDC = 0,      // Network Control Interface
  ITF_NUM_CDC_DATA,     // Network Data Interface
  ITF_NUM_CDC_ACM,      // ACM Control Interface
  ITF_NUM_CDC_ACM_DATA, // ACM Data Interface
  ITF_NUM_DFU_RT,       // DFU Runtime Interface
#if USB_CDC_SPLC
  ITF_NUM_CDC_SPLC,     // SimplePLC ACM Control Interface
  ITF_NUM_CDC_SPLC_DATA,// SimplePLC ACM Data Interface
#endif
  ITF_NUM_TOTAL
};

enum {
#if CFG_TUD_ECM_RNDIS
  CONFIG_ID_RNDIS = 0,
  CONFIG_ID_ECM   = 1,
#else
  CONFIG_ID_NCM = 0,
#endif
  CONFIG_ID_COUNT
};

#if CFG_TUD_NCM
#define USB_BCD 0x0201
#else
#define USB_BCD 0x0200
#endif

//--------------------------------------------------------------------+
// Device Descriptors
//--------------------------------------------------------------------+
static const tusb_desc_device_t desc_device = {
  .bLength         = sizeof(tusb_desc_device_t),
  .bDescriptorType = TUSB_DESC_DEVICE,
  .bcdUSB          = USB_BCD,
  // Use Interface Association Descriptor (IAD) device class
  .bDeviceClass    = TUSB_CLASS_MISC,
  .bDeviceSubClass = MISC_SUBCLASS_COMMON,
  .bDeviceProtocol = MISC_PROTOCOL_IAD,

  .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,

  .idVendor  = 0xCafe,
  .idProduct = USB_PID,
  .bcdDevice = 0x0102,   // was 0x0101: a new interface was added, lets Windows treat it as a new revision

  .iManufacturer = STRID_MANUFACTURER,
  .iProduct      = STRID_PRODUCT,
  .iSerialNumber = STRID_SERIAL,

  .bNumConfigurations = CONFIG_ID_COUNT // multiple configurations
};

// Invoked when received GET DEVICE DESCRIPTOR
// Application return pointer to descriptor
const uint8_t *tud_descriptor_device_cb(void) {
  return (const uint8_t *)&desc_device;
}

//--------------------------------------------------------------------+
// Configuration Descriptor
//--------------------------------------------------------------------+
#if USB_CDC_SPLC
#define SPLC_CDC_DESC_LEN TUD_CDC_DESC_LEN
#else
#define SPLC_CDC_DESC_LEN 0
#endif
#define MAIN_CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_RNDIS_DESC_LEN + TUD_CDC_DESC_LEN + TUD_DFU_RT_DESC_LEN + SPLC_CDC_DESC_LEN)
#define ALT_CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_CDC_ECM_DESC_LEN + TUD_CDC_DESC_LEN + TUD_DFU_RT_DESC_LEN + SPLC_CDC_DESC_LEN)
#define NCM_CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_CDC_NCM_DESC_LEN + TUD_CDC_DESC_LEN + TUD_DFU_RT_DESC_LEN + SPLC_CDC_DESC_LEN)

// Safe Explicit Endpoint Routing for STM32H5 Shared-Direction Architecture
#define EPNUM_NET_NOTIF 0x81
#define EPNUM_NET_OUT   0x02
#define EPNUM_NET_IN    0x82

#define EPNUM_ACM_NOTIF 0x83
#define EPNUM_ACM_OUT   0x04
#define EPNUM_ACM_IN    0x84

// SimplePLC port: EP5 = interrupt notification (IN only), EP6 = bulk OUT/IN. STM32H5 USB_DRD_FS has
// 8 endpoint numbers (FSDEV_EP_COUNT 8), 1..4 are used above; IN/OUT of one number share a type, so
// the notification endpoint gets its own number, same as the two ports above.
#define EPNUM_SPLC_NOTIF 0x85
#define EPNUM_SPLC_OUT   0x06
#define EPNUM_SPLC_IN    0x86

// The trailing comma is part of the macro so that "off" leaves a valid initializer list.
#if USB_CDC_SPLC
#define SPLC_CDC_FS_BLOCK \
  TUD_CDC_DESCRIPTOR(ITF_NUM_CDC_SPLC, STRID_CDC_SPLC, EPNUM_SPLC_NOTIF, 8, EPNUM_SPLC_OUT, EPNUM_SPLC_IN, 64),
#define SPLC_CDC_HS_BLOCK \
  TUD_CDC_DESCRIPTOR(ITF_NUM_CDC_SPLC, STRID_CDC_SPLC, EPNUM_SPLC_NOTIF, 8, EPNUM_SPLC_OUT, EPNUM_SPLC_IN, 512),
#else
#define SPLC_CDC_FS_BLOCK
#define SPLC_CDC_HS_BLOCK
#endif

// DFU Runtime descriptor parameters
// Runtime mode has no endpoints of its own; it only advertises that the
// device can detach and re-enumerate into a DFU (bootloader) mode.
#define DFU_RT_ATTR           (DFU_ATTR_CAN_DOWNLOAD | DFU_ATTR_CAN_UPLOAD | DFU_ATTR_MANIFESTATION_TOLERANT)
#define DFU_RT_DETACH_TIMEOUT 1000 // ms
#define DFU_RT_XFER_SIZE      4096 // bytes, only relevant once in DFU mode

#if CFG_TUD_ECM_RNDIS

// full speed configuration
static uint8_t const rndis_fs_configuration[] = {
  // Config number (index+1), interface count, string index, total length, attribute, power in mA
  TUD_CONFIG_DESCRIPTOR(CONFIG_ID_RNDIS + 1, ITF_NUM_TOTAL, 0, MAIN_CONFIG_TOTAL_LEN, 0, 100),

  // Interface number, string index, EP notification address and size, EP data address (out, in) and size.
  TUD_RNDIS_DESCRIPTOR(
      ITF_NUM_CDC, STRID_INTERFACE, EPNUM_NET_NOTIF, 8, EPNUM_NET_OUT, EPNUM_NET_IN, 64),

  // ACM Port full speed macro block
  TUD_CDC_DESCRIPTOR(
      ITF_NUM_CDC_ACM, STRID_CDC_ACM, EPNUM_ACM_NOTIF, 8, EPNUM_ACM_OUT, EPNUM_ACM_IN, 64),

  // Interface number, string index, attributes, detach timeout, xfer size
  TUD_DFU_RT_DESCRIPTOR(ITF_NUM_DFU_RT, STRID_DFU_RT, DFU_RT_ATTR, DFU_RT_DETACH_TIMEOUT, DFU_RT_XFER_SIZE),

  // SimplePLC App link (CDC instance 1)
  SPLC_CDC_FS_BLOCK
};
TU_VERIFY_STATIC(sizeof(rndis_fs_configuration) == MAIN_CONFIG_TOTAL_LEN, "rndis_fs_configuration: descriptor length mismatch");

static const uint8_t ecm_fs_configuration[] = {
  // Config number (index+1), interface count, string index, total length, attribute, power in mA
  TUD_CONFIG_DESCRIPTOR(CONFIG_ID_ECM + 1, ITF_NUM_TOTAL, 0, ALT_CONFIG_TOTAL_LEN, 0, 100),

  // Interface number, description string index, MAC address string index, EP notification address and size, EP data address (out, in), and size, max segment size.
  TUD_CDC_ECM_DESCRIPTOR(
      ITF_NUM_CDC, STRID_INTERFACE, STRID_MAC, EPNUM_NET_NOTIF, 64, EPNUM_NET_OUT, EPNUM_NET_IN,
      64, CFG_TUD_NET_MTU),

  // ACM Port full speed macro block
  TUD_CDC_DESCRIPTOR(
      ITF_NUM_CDC_ACM, STRID_CDC_ACM, EPNUM_ACM_NOTIF, 8, EPNUM_ACM_OUT, EPNUM_ACM_IN, 64),

  // Interface number, string index, attributes, detach timeout, xfer size
  TUD_DFU_RT_DESCRIPTOR(ITF_NUM_DFU_RT, STRID_DFU_RT, DFU_RT_ATTR, DFU_RT_DETACH_TIMEOUT, DFU_RT_XFER_SIZE),

  // SimplePLC App link (CDC instance 1)
  SPLC_CDC_FS_BLOCK
};
TU_VERIFY_STATIC(sizeof(ecm_fs_configuration) == ALT_CONFIG_TOTAL_LEN, "ecm_fs_configuration: descriptor length mismatch");

#if TUD_OPT_HIGH_SPEED
// Per USB specs: high speed capable device must report device_qualifier and other_speed_configuration

// high speed configuration
static uint8_t const rndis_hs_configuration[] = {
  // Config number (index+1), interface count, string index, total length, attribute, power in mA
  TUD_CONFIG_DESCRIPTOR(CONFIG_ID_RNDIS + 1, ITF_NUM_TOTAL, 0, MAIN_CONFIG_TOTAL_LEN, 0, 100),

  // Interface number, string index, EP notification address and size, EP data address (out, in) and size.
  TUD_RNDIS_DESCRIPTOR(
      ITF_NUM_CDC, STRID_INTERFACE, EPNUM_NET_NOTIF, 8, EPNUM_NET_OUT, EPNUM_NET_IN, 512),

  // ACM Port high speed macro block
  TUD_CDC_DESCRIPTOR(
      ITF_NUM_CDC_ACM, STRID_CDC_ACM, EPNUM_ACM_NOTIF, 8, EPNUM_ACM_OUT, EPNUM_ACM_IN, 512),

  // Interface number, string index, attributes, detach timeout, xfer size
  TUD_DFU_RT_DESCRIPTOR(ITF_NUM_DFU_RT, STRID_DFU_RT, DFU_RT_ATTR, DFU_RT_DETACH_TIMEOUT, DFU_RT_XFER_SIZE),

  // SimplePLC App link (CDC instance 1)
  SPLC_CDC_HS_BLOCK
};
TU_VERIFY_STATIC(sizeof(rndis_hs_configuration) == MAIN_CONFIG_TOTAL_LEN, "rndis_hs_configuration: descriptor length mismatch");

static const uint8_t ecm_hs_configuration[] = {
  // Config number (index+1), interface count, string index, total length, attribute, power in mA
  TUD_CONFIG_DESCRIPTOR(CONFIG_ID_ECM + 1, ITF_NUM_TOTAL, 0, ALT_CONFIG_TOTAL_LEN, 0, 100),

  // Interface number, description string index, MAC address string index, EP notification address and size, EP data address (out, in), and size, max segment size.
  TUD_CDC_ECM_DESCRIPTOR(
      ITF_NUM_CDC, STRID_INTERFACE, STRID_MAC, EPNUM_NET_NOTIF, 64, EPNUM_NET_OUT, EPNUM_NET_IN,
      512, CFG_TUD_NET_MTU),

  // ACM Port high speed macro block
  TUD_CDC_DESCRIPTOR(
      ITF_NUM_CDC_ACM, STRID_CDC_ACM, EPNUM_ACM_NOTIF, 8, EPNUM_ACM_OUT, EPNUM_ACM_IN, 512),

  // Interface number, string index, attributes, detach timeout, xfer size
  TUD_DFU_RT_DESCRIPTOR(ITF_NUM_DFU_RT, STRID_DFU_RT, DFU_RT_ATTR, DFU_RT_DETACH_TIMEOUT, DFU_RT_XFER_SIZE),

  // SimplePLC App link (CDC instance 1)
  SPLC_CDC_HS_BLOCK
};
TU_VERIFY_STATIC(sizeof(ecm_hs_configuration) == ALT_CONFIG_TOTAL_LEN, "ecm_hs_configuration: descriptor length mismatch");
#endif // highspeed

#else

// full speed configuration
static uint8_t const ncm_fs_configuration[] = {
  // Config number (index+1), interface count, string index, total length, attribute, power in mA
  TUD_CONFIG_DESCRIPTOR(CONFIG_ID_NCM + 1, ITF_NUM_TOTAL, 0, NCM_CONFIG_TOTAL_LEN, 0, 100),

  // Interface number, description string index, MAC address string index, EP notification address and size, EP data address (out, in), and size, max segment size, EP notification bInterval, NCM capabilities.
  TUD_CDC_NCM_DESCRIPTOR(ITF_NUM_CDC, STRID_INTERFACE, STRID_MAC, EPNUM_NET_NOTIF, 64, EPNUM_NET_OUT, EPNUM_NET_IN,
    64, CFG_TUD_NET_MTU, 50, (uint8_t)((uint8_t)NCM_NETWORK_CAPS_ETH_FILTER | (uint8_t)NCM_NETWORK_CAPS_NTB_INPUT_SIZE)),

  // ACM Port full speed macro block
  TUD_CDC_DESCRIPTOR(
      ITF_NUM_CDC_ACM, STRID_CDC_ACM, EPNUM_ACM_NOTIF, 8, EPNUM_ACM_OUT, EPNUM_ACM_IN, 64),

  // Interface number, string index, attributes, detach timeout, xfer size
  TUD_DFU_RT_DESCRIPTOR(ITF_NUM_DFU_RT, STRID_DFU_RT, DFU_RT_ATTR, DFU_RT_DETACH_TIMEOUT, DFU_RT_XFER_SIZE),

  // SimplePLC App link (CDC instance 1)
  SPLC_CDC_FS_BLOCK
};
TU_VERIFY_STATIC(sizeof(ncm_fs_configuration) == NCM_CONFIG_TOTAL_LEN, "ncm_fs_configuration: descriptor length mismatch");

#if TUD_OPT_HIGH_SPEED
// Per USB specs: high speed capable device must report device_qualifier and other_speed_configuration

// high speed configuration
// bInterval: FS=50 means 50ms; HS encodes as 2^(n-1) * 125us, so 9 = 2^8 * 125us = 32ms
static uint8_t const ncm_hs_configuration[] = {
  // Config number (index+1), interface count, string index, total length, attribute, power in mA
  TUD_CONFIG_DESCRIPTOR(CONFIG_ID_NCM + 1, ITF_NUM_TOTAL, 0, NCM_CONFIG_TOTAL_LEN, 0, 100),

  // Interface number, description string index, MAC address string index, EP notification address and size, EP data address (out, in), and size, max segment size, EP notification bInterval, NCM capabilities.
  TUD_CDC_NCM_DESCRIPTOR(ITF_NUM_CDC, STRID_INTERFACE, STRID_MAC, EPNUM_NET_NOTIF, 64, EPNUM_NET_OUT, EPNUM_NET_IN,
    512, CFG_TUD_NET_MTU, 9, (uint8_t)((uint8_t)NCM_NETWORK_CAPS_ETH_FILTER | (uint8_t)NCM_NETWORK_CAPS_NTB_INPUT_SIZE)),

  // ACM Port high speed macro block
  TUD_CDC_DESCRIPTOR(
      ITF_NUM_CDC_ACM, STRID_CDC_ACM, EPNUM_ACM_NOTIF, 8, EPNUM_ACM_OUT, EPNUM_ACM_IN, 512),

  // Interface number, string index, attributes, detach timeout, xfer size
  TUD_DFU_RT_DESCRIPTOR(ITF_NUM_DFU_RT, STRID_DFU_RT, DFU_RT_ATTR, DFU_RT_DETACH_TIMEOUT, DFU_RT_XFER_SIZE),

  // SimplePLC App link (CDC instance 1)
  SPLC_CDC_HS_BLOCK
};
TU_VERIFY_STATIC(sizeof(ncm_hs_configuration) == NCM_CONFIG_TOTAL_LEN, "ncm_hs_configuration: descriptor length mismatch");
#endif // highspeed

#endif

// NCM work with all latest OS i.e macos 10.10+, windows 10+, and Linux.
// For older system Configuration array of RNDIS and CDC-ECM may be needed for better compatibility.
// - Windows only works with RNDIS
// - MacOS only works with CDC-ECM
// - Linux will work on both
#if CFG_TUD_ECM_RNDIS

static const uint8_t *const configuration_fs_arr[CONFIG_ID_COUNT] = {
  [CONFIG_ID_RNDIS] = rndis_fs_configuration,
  [CONFIG_ID_ECM]   = ecm_fs_configuration
};

#if TUD_OPT_HIGH_SPEED
static const uint8_t *const configuration_hs_arr[CONFIG_ID_COUNT] = {
  [CONFIG_ID_RNDIS] = rndis_hs_configuration,
  [CONFIG_ID_ECM]   = ecm_hs_configuration
};

// Size array for each configuration
static const uint16_t configuration_sz_arr[CONFIG_ID_COUNT] = {
  [CONFIG_ID_RNDIS] = MAIN_CONFIG_TOTAL_LEN,
  [CONFIG_ID_ECM]   = ALT_CONFIG_TOTAL_LEN
};

// Scratch buffer for other speed configuration (sized to hold the largest config)
#define MAX_CONFIG_TOTAL_LEN TU_MAX(MAIN_CONFIG_TOTAL_LEN, ALT_CONFIG_TOTAL_LEN)
#endif

#else

static const uint8_t *const configuration_fs_arr[CONFIG_ID_COUNT] = {
  [CONFIG_ID_NCM] = ncm_fs_configuration
};

#if TUD_OPT_HIGH_SPEED
static const uint8_t *const configuration_hs_arr[CONFIG_ID_COUNT] = {
  [CONFIG_ID_NCM] = ncm_hs_configuration
};

// Size array for each configuration
static const uint16_t configuration_sz_arr[CONFIG_ID_COUNT] = {
  [CONFIG_ID_NCM] = NCM_CONFIG_TOTAL_LEN
};

// Scratch buffer for other speed configuration (sized to hold the largest config)
#define MAX_CONFIG_TOTAL_LEN NCM_CONFIG_TOTAL_LEN
#endif

#endif

#if TUD_OPT_HIGH_SPEED
static uint8_t desc_other_speed_config[MAX_CONFIG_TOTAL_LEN];

// device qualifier: device descriptor fields that differ at other speed
static tusb_desc_device_qualifier_t const desc_device_qualifier = {
  .bLength            = sizeof(tusb_desc_device_qualifier_t),
  .bDescriptorType    = TUSB_DESC_DEVICE_QUALIFIER,
  .bcdUSB             = USB_BCD,

  .bDeviceClass       = TUSB_CLASS_MISC,
  .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
  .bDeviceProtocol    = MISC_PROTOCOL_IAD,

  .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
  .bNumConfigurations = CONFIG_ID_COUNT,
  .bReserved          = 0x00
};

// Invoked when received GET DEVICE QUALIFIER DESCRIPTOR request
uint8_t const *tud_descriptor_device_qualifier_cb(void) {
  return (uint8_t const *) &desc_device_qualifier;
}

// Invoked when received GET OTHER SPEED CONFIGURATION DESCRIPTOR request
uint8_t const *tud_descriptor_other_speed_configuration_cb(uint8_t index) {
  if (index >= CONFIG_ID_COUNT) return NULL;

  const uint8_t *const *arr = (tud_speed_get() == TUSB_SPEED_HIGH) ? configuration_fs_arr : configuration_hs_arr;

  memcpy(desc_other_speed_config, arr[index], configuration_sz_arr[index]);
  desc_other_speed_config[1] = TUSB_DESC_OTHER_SPEED_CONFIG;

  return desc_other_speed_config;
}

#endif // highspeed

// Invoked when received GET CONFIGURATION DESCRIPTOR
const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
  if (index >= CONFIG_ID_COUNT) return NULL;
#if TUD_OPT_HIGH_SPEED
  return (tud_speed_get() == TUSB_SPEED_HIGH) ? configuration_hs_arr[index] : configuration_fs_arr[index];
#else
  return configuration_fs_arr[index];
#endif
}

#if CFG_TUD_NCM
//--------------------------------------------------------------------+
// BOS Descriptor
//--------------------------------------------------------------------+
#define BOS_TOTAL_LEN     (TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN)
#define MS_OS_20_DESC_LEN 0xB2

const uint8_t desc_bos[] = {
  TUD_BOS_DESCRIPTOR(BOS_TOTAL_LEN, 1),
  TUD_BOS_MICROSOFT_OS_20_DESCRIPTOR(MS_OS_20_DESC_LEN, 1)};

const uint8_t *tud_descriptor_bos_cb(void) {
  return desc_bos;
}

const uint8_t desc_ms_os_20[] = {
  U16_TO_U8S_LE(0x000A), U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR), U32_TO_U8S_LE(0x06030000),
  U16_TO_U8S_LE(MS_OS_20_DESC_LEN),

  U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_CONFIGURATION), 0, 0,
  U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A),

  U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION), ITF_NUM_CDC, 0,
  U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A - 0x08),

  U16_TO_U8S_LE(0x0014), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID), 'W', 'I', 'N', 'N', 'C', 'M', 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

  U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A - 0x08 - 0x08 - 0x14), U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY),
  U16_TO_U8S_LE(0x0007),
  U16_TO_U8S_LE(0x002A),
  'D', 0x00, 'e', 0x00, 'v', 0x00, 'i', 0x00, 'c', 0x00, 'e', 0x00, 'I', 0x00, 'n', 0x00, 't', 0x00, 'e', 0x00, 'r',
  0x00, 'f', 0x00, 'a', 0x00, 'c', 0x00, 'e', 0x00, 'G', 0x00, 'U', 0x00, 'I', 0x00, 'D', 0x00, 's', 0x00, 0x00, 0x00,
  U16_TO_U8S_LE(0x0050),
  '{', 0x00, '1', 0x00, '2', 0x00, '3', 0x00, '4', 0x00, '5', 0x00, '6', 0x00, '7', 0x00, '8', 0x00, '-', 0x00, '0',
  0x00, 'D', 0x00, '0', 0x00, '8', 0x00, '-', 0x00, '4', 0x00, '3', 0x00, 'F', 0x00, 'D', 0x00, '-', 0x00, '8', 0x00,
  'B', 0x00, '3', 0x00, 'E', 0x00, '-', 0x00, '1', 0x00, '2', 0x00, '7', 0x00, 'C', 0x00, 'A', 0x00, '8', 0x00, 'A',
  0x00, 'F', 0x00, 'F', 0x00, 'F', 0x00, '9', 0x00, 'D', 0x00, '}', 0x00, 0x00, 0x00, 0x00, 0x00};

TU_VERIFY_STATIC(sizeof(desc_ms_os_20) == MS_OS_20_DESC_LEN, "Incorrect size");

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, const tusb_control_request_t *request) {
  if (stage != CONTROL_STAGE_SETUP) {
    return true;
  }

  switch (request->bmRequestType_bit.type) {
    case TUSB_REQ_TYPE_VENDOR:
      switch (request->bRequest) {
        case 1:
          if (request->wIndex == 7) {
            uint16_t total_len;
            memcpy(&total_len, desc_ms_os_20 + 8, 2);
            return tud_control_xfer(rhport, request, (void *)(uintptr_t)desc_ms_os_20, total_len);
          } else {
            return false;
          }
        default:
          break;
      }
      break;
    default:
      break;
  }
  return false;
}

#endif

//--------------------------------------------------------------------+
// String Descriptors
//--------------------------------------------------------------------+

static const char *string_desc_arr[STRID_COUNT] = {
  [STRID_LANGID]       = (const char[]){0x09, 0x04},  // supported language is English (0x0409)
  [STRID_MANUFACTURER] = "Synaptix.,JSC",             // Manufacturer
  [STRID_PRODUCT]      = "Synaptix RemoteIO",            // Product
  [STRID_SERIAL]       = "15FC26ab31204EA",                         // Serials will use unique ID if possible
  [STRID_INTERFACE]    = "Synaptix Network Interface", // Interface Description
  [STRID_MAC]          = NULL,                         // STRID_MAC index is handled separately
  [STRID_CDC_ACM]      = "Synaptix COM Port",          // Description for Virtual COM interface
  [STRID_DFU_RT]        = "Synaptix DFU",                // Description for DFU Runtime interface
  [STRID_CDC_SPLC]      = "Synaptix PLC Port"            // SimplePLC App link (CDC instance 1)
};

static uint32_t cached_uid[3];
size_t board_get_unique_id(uint8_t id[], size_t max_len) {
  (void) max_len;
  uint32_t* id32 = (uint32_t*) (uintptr_t) id;
  uint8_t const len = 12;

  id32[0] = cached_uid[0];
  id32[1] = cached_uid[1];
  id32[2] = cached_uid[2];

  return len;
}

size_t board_usb_get_serial(uint16_t desc_str1[], size_t max_chars) {
  uint8_t uid[16] TU_ATTR_ALIGNED(4);
  size_t uid_len;

  uid_len = board_get_unique_id(uid, sizeof(uid));

  if ( uid_len > max_chars / 2u ) {
    uid_len = max_chars / 2u;
  }

  for ( size_t i = 0; i < uid_len; i++ ) {
    for ( size_t j = 0; j < 2; j++ ) {
      const unsigned char nibble_to_hex[16] = {
          '0', '1', '2', '3', '4', '5', '6', '7',
          '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'
      };
      const uint8_t nibble = (uint8_t) ((uid[i] >> (j * 4u)) & 0xfu);
      desc_str1[i * 2 + (1 - j)] = nibble_to_hex[nibble]; // UTF-16-LE
    }
  }

  return 2 * uid_len;
}

static uint16_t _desc_str[32 + 1];

// Invoked when received GET STRING DESCRIPTOR request
const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    uint8_t chr_count;

    if (index == 0) {
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else {
        if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0]))
            return NULL;
        const char *str = string_desc_arr[index];
        chr_count = (uint8_t)strlen(str);
        if (chr_count > 31) chr_count = 31;
        for (uint8_t i = 0; i < chr_count; i++)
            _desc_str[1 + i] = str[i];
    }
    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}

//--------------------------------------------------------------------+
// DFU Runtime callbacks
//--------------------------------------------------------------------+
#if CFG_TUD_DFU_RUNTIME

// Invoked when received DFU_DETACH request from host, host will
// expect the device to reset or re-enumerate into DFU (bootloader) mode
// within DFU_RT_DETACH_TIMEOUT ms.
// #include "bootloader.h"
#include "stm32h5xx_hal.h"
// extern Bootloader_t bootloader;
void tud_dfu_runtime_reboot_to_dfu_cb(void) {
  // TODO: set a persistent flag (e.g. backup register / no-init RAM)
  // that the bootloader checks on the next boot, then trigger a
  // system reset so the device re-enumerates in DFU mode, e.g.:
  //   dfu_request_boot_into_bootloader();
  //   NVIC_SystemReset();
  // bootloader.boot_flash.partition.isUpgradeInProgress = true;
  //   bootloader_save_partition(&bootloader);
    NVIC_SystemReset();
}

#endif