# port/CMakeLists.txt - Layer 3.5 (Protocol / Library Porting)
#
# Builds the porting-layer glue code that adapts third-party libraries
# (currently only nanoMODBUS) to this project's Layer 1 components, as a
# static library target: splc_port.
#
# Per docs/architecture.md section 1 / section 3 ("Bang tong hop - Ai
# duoc include ai"), Layer 3.5 sits between Layer 3 (services/) and
# Layer 1 (components/): each port/<name>/ subdirectory wraps exactly one
# transport (USB, RS485 serial, TCP) behind the read()/write() function-
# pointer shape libs/nanomodbus/nanomodbus.h expects in
# nmbs_platform_conf. Each subdirectory is independent -- none of them
# include each other, and only plc_modbus_cfg.c (Layer 3) is expected to
# include any of their headers directly.
#
# Real content today: port/modbus_link/ (the generic App<->device byte pipe,
# lwIP-netif style) and port/modbus_serial/ (the Gateway's RS485 master).
# port/modbus_tcp/ is still an empty stub (NOTE below) and is left out of
# SPLC_PORT_SRC.
#
# The USB-CDC transport is NOT part of this target: it is the optional module
# port/usb/ (target splc_port_usb_cdc), built only when SPLC_PORT_USB_CDC = 1
# in splcopts.h -- see the add_subdirectory(usb) at the end of this file.
set(SPLC_PORT_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/modbus_serial/modbus_serial.c
    ${CMAKE_CURRENT_SOURCE_DIR}/modbus_link/modbus_link.c
)

# modbus_link (Layer 3.5): the generic App<->device byte pipe, lwIP-netif
# style (splc_link_register / splc_link_input / output()). The core owns the
# RX ring and every timeout; the product's port file only moves bytes. It has
# no dependency on any transport driver or third-party library (only
# sx_time.h / sx_os.h from splc_components), so it is compiled into every
# build (measured on ARM Release: ~950 bytes of code, ~30 bytes of RAM plus the
# SPLC_LINK_RX_BUF_SIZE ring; the linker drops all of it while nothing calls
# it). The App link of a board is plugged into it with splc_link_register()
# (the USB-CDC port module does that for the Zigbee-IO board).

# modbus_serial (Layer 3.5): non-blocking Modbus RTU MASTER over sx_uart_t
# (RS485), used by the Gateway's Remote I/O channel (services/plc_io/
# plc_io_remote.c). It does not use nanoMODBUS's blocking client; only
# nmbs_crc_calc() is taken from nanoMODBUS. Compiled into every build (a few
# hundred bytes of code, no static RAM until a master is created).
#
# NOTE: port/modbus_tcp/modbus_tcp.c is NOT in SPLC_PORT_SRC, and
# is further along from "stub" -- it is a genuinely EMPTY file (0 bytes,
# not even an #include). port/modbus_tcp/modbus_tcp.h itself has no
# function declarations yet either (just the include guard and
# #include "nanomodbus.h"), so there is nothing here yet to wrap Layer 1
# around (a Gateway variant's Ethernet/WiFi Modbus TCP transport isn't
# designed at the components/ level yet, per docs/handoff.md's "still
# open" list). Left out entirely rather than added as an empty
# translation unit.

add_library(splc_port STATIC ${SPLC_PORT_SRC})

# PUBLIC: plc_modbus_cfg.c (Layer 3, services/plc_modbus_cfg/) resolves
# modbus_transport.h directly to build the modbus_transport_t it consumes --
# see modbus_transport.h's own header comment for the full
# transport-agnostic design.
target_include_directories(splc_port PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/modbus_serial
    ${CMAKE_CURRENT_SOURCE_DIR}/modbus_link
    ${CMAKE_CURRENT_SOURCE_DIR}/modbus_transport
)

# splc_components (Layer 1) supplies sx_time.h / sx_os.h / sx_uart.h, which
# modbus_link.c and modbus_serial.c include. PUBLIC for the same reason as in
# platforms/stm32/stm32h5/CMakeLists.txt.
target_link_libraries(splc_port PUBLIC
    splc_components
)

# modbus_serial.c calls nmbs_crc_calc() (libs/splc_nanomodbus), so splc_port
# links the splc_nanomodbus target (libs/ is added before port/ in the root
# CMakeLists). sx_time.h (Layer 1) comes from splc_components.
target_link_libraries(splc_port PUBLIC
    splc_nanomodbus
)

target_compile_features(splc_port PUBLIC c_std_11)

# --- Optional module: USB-CDC App link (port/usb/) ------------------------
#
# SPLC_PORT_USB_CDC (splcopts.h, read by cmake_modules/splc_select.cmake).
# 1: port/usb/CMakeLists.txt defines splc_port_usb_cdc (TinyUSB glue + the
#    USB-CDC pipe registered with modbus_link). It links splc_port, so it comes
#    after the splc_port definition above; the board target links it.
# 0: no USB code is built at all (the host firmware owns the USB peripheral).
if(SPLC_PORT_USB_CDC)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/usb)
endif()