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

# PUBLIC: plc_modbus_cfg.c (Layer 3, not yet written) will need to
# resolve modbus_usb.h directly to build the nmbs_platform_conf it passes
# to nmbs_client_create()/nmbs_server_create() -- see the "Ownership /
# arg convention" comment in modbus_usb.h for how that wiring is meant to
# work (arg = a sx_usb_tiny_t* the caller owns, not something hidden in
# this library).
target_include_directories(splc_port PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/modbus_usb
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

# TODO(wiring): modbus_usb.c and modbus_usb.h both #include
# "nanomodbus.h" is NOT actually true today -- double-checked: neither
# file includes it directly, they only implement the two function
# signatures nanoMODBUS's nmbs_platform_conf expects, without needing any
# nanoMODBUS type themselves (int32_t/uint8_t/uint16_t are enough). So
# splc_port does NOT strictly need to link a "nanomodbus" target to
# compile as of this file.
#
# It WILL need one at the next step, though: plc_modbus_cfg.c (Layer 3)
# is the file that actually populates an nmbs_platform_conf with
# modbus_usb_read/modbus_usb_write and calls nmbs_client_create()/
# nmbs_server_create(), so IT needs nanomodbus.h resolvable and
# nanomodbus.c's ~2460 lines actually compiled into some target.
#
# libs/CMakeLists.txt (libs/nanomodbus/) is still empty today -- no
# target there yet exposes nanomodbus.h or compiles nanomodbus.c. This
# file deliberately does NOT reach into libs/ and wire that up itself
# (that is a separate layer's CMakeLists.txt to own, not this one's) --
# flagging it here instead: once libs/CMakeLists.txt defines a target
# (e.g. add_library(nanomodbus STATIC libs/nanomodbus/nanomodbus.c) with
# PUBLIC target_include_directories(... libs/nanomodbus)), Layer 3's
# CMakeLists.txt (services/CMakeLists.txt, not yet created) should link
# splc_services against both splc_port and that nanomodbus target --
# splc_port itself has no reason to depend on nanomodbus given what it
# actually contains right now.

target_compile_features(splc_port PUBLIC c_std_11)