# port/usb/CMakeLists.txt - optional module: USB-CDC App link (Layer 3.5)
#
# Added by port/CMakeLists.txt only when SPLC_PORT_USB_CDC = 1 (splcopts.h).
# With 0 nothing in this directory is built and no TinyUSB exists in the
# firmware: the host owns the USB peripheral and feeds the App link itself
# (port/modbus_link, docs/architecture.md section 3.1).
#
# What the module is:
#   splc_usb_cdc.c     the pipe: splc_usb_cdc_init() starts TinyUSB and registers
#                      the USB-CDC link with modbus_link (splc_link_register);
#                      splc_usb_irq() is the interrupt entry the board calls
#   tusb_config.h,     TinyUSB configuration and the CDC-only descriptors; they
#   usb_descriptors.c  are compiled into the "tinyusb" target (libs/CMakeLists.txt)
#
# Only this module may #include "tusb.h" / call tud_*().

add_library(splc_port_usb_cdc STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/splc_usb_cdc.c
)

# PUBLIC: the board file includes splc_usb_cdc.h.
target_include_directories(splc_port_usb_cdc PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# tinyusb (libs/CMakeLists.txt): tusb.h, the stm32_fsdev driver and
# tusb_config.h. splc_port: modbus_link.h / modbus_transport.h and the link
# itself. splc_components: sx_time.h / sx_os.h / logger include dir.
target_link_libraries(splc_port_usb_cdc PUBLIC
    tinyusb
    splc_port
    splc_components
)

target_compile_features(splc_port_usb_cdc PUBLIC c_std_11)