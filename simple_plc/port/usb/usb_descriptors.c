#include "tusb.h"
#include <string.h>

/*  UID Serial  */
// static size_t get_serial_from_uid(uint16_t *buf, size_t buf_len)
// {
//     uint32_t const uid[3] = {
//         *(volatile uint32_t *)(0x08FFF800UL),
//         *(volatile uint32_t *)(0x08FFF804UL),
//         *(volatile uint32_t *)(0x08FFF808UL)
//     };
//     const char hex[] = "0123456789ABCDEF";
//     size_t count = 0;
//     for (int w = 0; w < 3 && count < buf_len; w++)
//         for (int b = 7; b >= 0 && count < buf_len; b--)
//             buf[count++] = hex[(uid[w] >> (b * 4)) & 0xF];
//     return count;
// }

/*  VID / PID  */
#define PID_MAP(itf, n)  ((CFG_TUD_##itf) ? (1 << (n)) : 0)
#define USB_PID  (0x4000 | PID_MAP(CDC,0) | PID_MAP(MSC,1) | \
                  PID_MAP(HID,2) | PID_MAP(MIDI,3) | PID_MAP(VENDOR,4))
#define USB_VID  0xCafe//0xCafe
#define USB_BCD  0x0200

/*  Device Descriptor  */
static tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = USB_BCD,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *)&desc_device;
}

/*  HID Report Descriptor (chỉ compile khi CFG_TUD_HID = 1)  */
#if CFG_TUD_HID
static const uint8_t hid_report_desc[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(1)),
    TUD_HID_REPORT_DESC_MOUSE   (HID_REPORT_ID(2)),
};

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    return hid_report_desc;
}
#endif

/*  Interface Numbers  */
enum {
    ITF_NUM_CDC_0 = 0,
    ITF_NUM_CDC_0_DATA,
#if CFG_TUD_HID
    ITF_NUM_HID,
#endif
#if CFG_TUD_MSC
    ITF_NUM_MSC,             /* MSC */
    ITF_NUM_TOTAL
#endif
};

/*  Endpoints  */
#define EPNUM_CDC_0_NOTIF   0x81   // EP1 IN  – CDC notification
#define EPNUM_CDC_0_OUT     0x02   // EP2 OUT – CDC data
#define EPNUM_CDC_0_IN      0x82   // EP2 IN  – CDC data

/*  MSC  */
#if CFG_TUD_MSC
#define EPNUM_MSC_OUT       0x03
#define EPNUM_MSC_IN        0x83
#endif

#if CFG_TUD_HID
#define EPNUM_HID           0x83   // EP3 IN  – HID
#endif

/*  Config Descriptor total length  */
#if CFG_TUD_MSC
#define CONFIG_TOTAL_LEN  ( TUD_CONFIG_DESC_LEN                      \
                          + TUD_CDC_DESC_LEN                         \
                          + (CFG_TUD_HID ? TUD_HID_DESC_LEN : 0)   \
                          + (CFG_TUD_MSC ? TUD_MSC_DESC_LEN : 0) )
#endif

/*  Configuration Descriptor  */
static uint8_t const desc_fs_configuration[] = {
#if CFG_TUD_MSC
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
#endif
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC_0, 4, EPNUM_CDC_0_NOTIF, 8,
                       EPNUM_CDC_0_OUT, EPNUM_CDC_0_IN, 64),

#if CFG_TUD_MSC
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 6, EPNUM_MSC_OUT, EPNUM_MSC_IN, CFG_TUD_MSC_EP_BUFSIZE),
#endif

#if CFG_TUD_HID
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, 5,
                       HID_ITF_PROTOCOL_NONE,
                       sizeof(hid_report_desc),
                       EPNUM_HID, 16, 5),
#endif
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_fs_configuration;
}

/*  String Descriptors  */
enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_CDC_ITF,    // index 4
#if CFG_TUD_HID
    STRID_HID_ITF,    // index 5
#endif
};

static char const *string_desc_arr[] = {
    [0] = (const char[]){ 0x09, 0x04 },
    [1] = "STMicroelectronics",
    [2] = "STM32H5 CDC"
          #if CFG_TUD_HID
          "+HID"
          #endif
          " Demo",
    [3] = NULL,                    // Serial from UID
    [4] = "TinyUSB CDC Port",
#if CFG_TUD_HID
    [5] = "TinyUSB HID KB+Mouse",
#endif
#if CFG_TUD_MSC
    [6] = "TRACKINGFW MSC Storage"
#endif
};

static uint16_t _desc_str[33];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    size_t chr_count;

    switch (index) {
        case STRID_LANGID:
            memcpy(&_desc_str[1], string_desc_arr[0], 2);
            chr_count = 1;
            break;

       case STRID_SERIAL: {
          const char *ser = "STM32H5_UID";   
          chr_count = strlen(ser);
          for (size_t i = 0; i < chr_count; i++)
              _desc_str[1 + i] = (uint16_t)ser[i];
          break;
      }

        default: {
            size_t arr_len = sizeof(string_desc_arr) / sizeof(string_desc_arr[0]);
            if (index >= arr_len || string_desc_arr[index] == NULL)
                return NULL;
            const char *str = string_desc_arr[index];
            chr_count = strlen(str);
            if (chr_count > 32) chr_count = 32;
            for (size_t i = 0; i < chr_count; i++)
                _desc_str[1 + i] = (uint16_t)str[i];
            break;
        }
    }

    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}