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
# Real content today: only port/modbus_usb/ (the App<->MCU channel over
# USB-CDC, per v1.7/v1.9 -- see docs/architecture.md section 2.4b).
# port/modbus_serial/ is the Gateway's RS485 master; port/modbus_tcp/ is
# still an empty stub (NOTE below) and is left out of SPLC_PORT_SRC.

# --- modbus_usb (Layer 3.5, has a real .c) -------------------------------
#
# modbus_usb_read()/modbus_usb_write() (port/modbus_usb/modbus_usb.c)
# call sx_usb_tiny_read()/sx_usb_tiny_write()/sx_usb_tiny_connected()
# (Layer 1, components/usb_cdc/) and are themselves called back by
# nanoMODBUS (libs/nanomodbus/) through nmbs_platform_conf function
# pointers, so this target needs both as dependencies (see
# target_link_libraries below).
set(SPLC_PORT_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/modbus_usb/modbus_usb.c
    ${CMAKE_CURRENT_SOURCE_DIR}/modbus_serial/modbus_serial.c
)

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
# modbus_usb.h and modbus_transport.h directly to build the
# modbus_transport_t it consumes -- see modbus_transport.h's own header
# comment for the full transport-agnostic design and "Ownership / arg
# convention" comment in modbus_usb.h for the underlying sx_usb_tiny_t*
# lifetime rule (caller-owned, not hidden in this library).
target_include_directories(splc_port PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/modbus_usb
    ${CMAKE_CURRENT_SOURCE_DIR}/modbus_serial
    ${CMAKE_CURRENT_SOURCE_DIR}/modbus_transport
)

# splc_components (Layer 1) supplies sx_usb_cdc.h, which modbus_usb.c
# includes directly (via modbus_usb.h -> sx_usb_cdc.h) and calls into
# (sx_usb_tiny_read/write/connected). PUBLIC for the same reason as in
# platforms/stm32/stm32h5/CMakeLists.txt: a caller consuming this
# library's headers (plc_modbus_cfg.c) transitively needs sx_usb_cdc.h
# resolvable too, since modbus_usb.h re-#includes it.
target_link_libraries(splc_port PUBLIC
    splc_components
)

# modbus_serial.c calls nmbs_crc_calc() (libs/nanomodbus), so splc_port links
# the nanomodbus target (libs/ is added before port/ in the root CMakeLists).
# modbus_usb.c/.h still do not include nanomodbus.h themselves.
# sx_time.h (Layer 1) comes from splc_components.
target_link_libraries(splc_port PUBLIC
    nanomodbus
)

target_compile_features(splc_port PUBLIC c_std_11)