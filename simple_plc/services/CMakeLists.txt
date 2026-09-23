# services/CMakeLists.txt - Layer 3 (PLC Application Services)
#
# Builds the hardware-dependent business-logic services that bridge
# Layer 2 (core/, hardware-agnostic Tag Table + Rule Engine) to real
# peripherals, as a static library target: splc_services.
#
# Per docs/architecture.md section 1, Layer 3 sits above Layer 2 and may
# freely include Layer 0/1 headers (sx_flash.h, sx_gpio.h, sx_adc.h,
# sx_time.h, splc_flash_define.h) -- unlike Layer 2, it is not required
# to build/test hardware-free.

set(SPLC_SERVICES_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/plc_io/plc_io.c
    ${CMAKE_CURRENT_SOURCE_DIR}/plc_retain/plc_retain.c
    ${CMAKE_CURRENT_SOURCE_DIR}/plc_modbus_cfg/plc_modbus_cfg.c
    ${CMAKE_CURRENT_SOURCE_DIR}/plc_rule_flash/plc_rule_flash.c
)

add_library(splc_services STATIC ${SPLC_SERVICES_SRC})

# PUBLIC: Layer 4 (app/plc_app/plc_engine.c, not yet written) will call
# into plc_io_register_di/do/ai(), input_scan(), output_scan(),
# retain_store_restore(), retain_service() -- all declared in these two
# headers -- so it needs them resolvable too, not just this target's own
# .c files.
target_include_directories(splc_services PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/plc_io
    ${CMAKE_CURRENT_SOURCE_DIR}/plc_retain
    ${CMAKE_CURRENT_SOURCE_DIR}/plc_modbus_cfg
    ${CMAKE_CURRENT_SOURCE_DIR}/plc_rule_flash
)

# splc_core (Layer 2) supplies plc_tag.h, which both plc_io.c and
# plc_retain.c include directly and call into (tag_read/tag_write/
# tag_get_kind/tag_vreg_retain_base_index). PUBLIC for the same transitive
# reason as elsewhere in this repo's CMakeLists.txt files: Layer 4 will
# also need plc_tag.h resolvable once it starts calling tag_read()/
# tag_write() itself alongside these services.
target_link_libraries(splc_services PUBLIC
    splc_core
)

# splc_components (Layer 1) supplies sx_gpio.h/sx_adc.h, which plc_io.h
# includes directly. STATIC library (usb_cdc/sx_usb_cdc.c is now wired
# in, see components/CMakeLists.txt) -- linking against it here works
# the same way syntactically whether it is STATIC or INTERFACE.
target_link_libraries(splc_services PUBLIC
    splc_components
)

# splc_platform_stm32h5 (Layer 0) supplies sx_flash.h/sx_time.h's actual
# implementations AND, transitively via its own PUBLIC
# target_include_directories(), splc_flash_define.h
# (platforms/stm32/stm32h5/flash_define/) and sx_platform_config.h
# (app/) -- both needed by plc_retain.c. This is why plc_retain.c/.h
# only need to #include "splc_flash_define.h" directly with no extra
# include-path wrangling on this target's part: linking
# splc_platform_stm32h5 PUBLIC here is what makes that resolve.
target_link_libraries(splc_services PUBLIC
    splc_platform_stm32h5
)

# nanomodbus (libs/CMakeLists.txt) supplies nanomodbus.h, which
# plc_retain.c includes directly (for nmbs_crc_calc(), reused as the
# CRC-16/MODBUS implementation for retain records rather than
# maintaining a second hand-written copy of the same algorithm -- see
# plc_retain.h's header comment). PUBLIC for the same transitive reason
# as the other dependencies here.
target_link_libraries(splc_services PUBLIC
    nanomodbus
)

# splc_port (Layer 3.5, port/CMakeLists.txt) supplies modbus_usb.h /
# modbus_usb_read() / modbus_usb_write(), which plc_modbus_cfg.c includes
# and calls directly (as nmbs_platform_conf.read/write) -- see
# port/modbus_usb/modbus_usb.h's own comment: "included by exactly one
# file: plc_modbus_cfg.c (Layer 3)".
target_link_libraries(splc_services PUBLIC
    splc_port
)

target_compile_features(splc_services PUBLIC c_std_11)