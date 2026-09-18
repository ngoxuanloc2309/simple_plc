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
# port/modbus_serial/ and port/modbus_tcp/ are still stubs (see the two
# NOTE blocks below) and are deliberately left out of
# SPLC_PORT_SRC/target_sources for now.

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
)

# NOTE: port/modbus_serial/modbus_serial.c is NOT in SPLC_PORT_SRC.
# It is a 2-line stub today (only #include "modbus_serial.h" and
# #include "nanomodbus.h", no function definitions at all) -- meant to
# eventually wrap sx_uart_read()/sx_uart_write() (Layer 1,
# components/uart/) for the Gateway variant's Modbus Master over real
# RS485, per docs/architecture.md's port/ table. Add it to SPLC_PORT_SRC
# once it has real read()/write() implementations; compiling it as-is
# would build fine (it defines nothing) but silently give a false sense
# that this transport is implemented when it is not.
#
# NOTE: port/modbus_tcp/modbus_tcp.c is NOT in SPLC_PORT_SRC either, and
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

# modbus_usb.c/.h still do NOT #include "nanomodbus.h" -- confirmed
# again while updating this comment: neither file includes it directly,
# they only implement the two function signatures nanoMODBUS's
# nmbs_platform_conf expects (int32_t/uint8_t/uint16_t are enough), so
# splc_port itself does not need to link "nanomodbus" to compile.
#
# nanomodbus.h resolvable + nanomodbus.c actually compiled is needed one
# layer up instead: services/CMakeLists.txt (now exists) links
# splc_services against both splc_port and the nanomodbus target
# (libs/CMakeLists.txt, now exists) -- see that file for the real
# wiring. This target intentionally still does not reach into libs/
# itself.

target_compile_features(splc_port PUBLIC c_std_11)