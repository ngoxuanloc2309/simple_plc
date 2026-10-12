# board/CMakeLists.txt - Layer 4 (board bring-up)
#
# Builds splc_board_init()/board_hw_init() as a static library target:
# splc_board. Only ONE SKU's board_<name>.c is compiled in at a time --
# selected from SPLC_BOARD in splcopts.h (cmake/splc_select.cmake sets SPLC_BOARD_SKU), not via
# #ifdef inside a shared file (see docs/handoff.md section 1.11's
# rationale against #ifdef-based SKU selection).
#
# NOTE: with SPLC_LOGGER_BUILTIN=1 (default, cmake_modules/splc_select.cmake)
# utils/logger/logger.c is included directly in this target's
# sources rather than getting its own utils/CMakeLists.txt target yet --
# board_<sku>.c is currently its only caller. Verified by direct gcc
# compile with FREE_RTOS unset (defaults to 0 per logger.h): 0 errors,
# only 2 harmless warnings (printf format-security, sign-compare) --
# the #include <FreeRTOS.h>/xSemaphore* calls are already guarded inside
# #if FREE_RTOS and never reached when building bare-metal. The root
# CMakeLists.txt's older comment claiming this file "still needs the
# FreeRTOS mutex stripped" is stale; no code change was needed here.
# With SPLC_LOGGER_BUILTIN=0 no logger.c is compiled and the host firmware
# supplies logger.h/logger (config/splc_log.h).

# SPLC_BOARD_SKU is set by cmake/splc_select.cmake from SPLC_BOARD in splcopts.h.

set(SPLC_BOARD_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/splc_board.c
    ${CMAKE_CURRENT_SOURCE_DIR}/board_device/board_${SPLC_BOARD_SKU}.c
    ${SPLC_LOGGER_SRC}      # empty when the host firmware supplies the logger (SPLC_LOGGER_BUILTIN=0)
)

add_library(splc_board STATIC ${SPLC_BOARD_SRC})

# board_device/ is public too: a SKU's own header (e.g.
# board_ethernet_wifi_gateway.h: splc_gateway_set_rs485_port()) is the API the
# host firmware's port file calls (port_gw/splc_port.c).
target_include_directories(splc_board PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/board_device
)

target_include_directories(splc_board PRIVATE
    ${SPLC_LOGGER_INCLUDE_DIR}                  # empty when the host supplies logger.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../app          # logger.c -> sx_os_config.h
)
if(SPLC_OS_INCLUDE_DIRS)                          # defined in platforms/freertos/CMakeLists.txt
    target_include_directories(splc_board PRIVATE ${SPLC_OS_INCLUDE_DIRS})
endif()

# splc_services (Layer 3) supplies plc_io.h (plc_io_register_di/do/ai()),
# which board_${SPLC_BOARD_SKU}.c calls directly.
target_link_libraries(splc_board PUBLIC
    splc_services
)

# splc_components (Layer 1) supplies sx_gpio.h/sx_uart.h, which
# board_${SPLC_BOARD_SKU}.c includes directly.
target_link_libraries(splc_board PUBLIC
    splc_components
)

# splc_port (Layer 3.5) supplies modbus_link.h (splc_link_get_transport()).
target_link_libraries(splc_board PUBLIC
    splc_port
)

# Optional USB-CDC port module (SPLC_PORT_USB_CDC = 1, splcopts.h): supplies
# splc_usb_cdc.h (splc_usb_cdc_init(), splc_usb_irq()) and brings TinyUSB in.
# Linked from here so it reaches the final executable through splc_board.
if(SPLC_PORT_USB_CDC)
    target_link_libraries(splc_board PUBLIC
        splc_port_usb_cdc
    )
endif()

# splc_core (Layer 2) supplies plc_tag.h (SPLC_TagLayout,
# tag_table_load_from_flash()), which board_${SPLC_BOARD_SKU}.c uses
# directly. TAG_DI0/TAG_DO0/etc. themselves now come from this board's own
# board_tag_define.h (board/, Layer 4), not from splc_core -- each board
# owns its own concrete tag indices, per its SPLC_TagLayout.
target_link_libraries(splc_board PUBLIC
    splc_core
)

target_compile_features(splc_board PUBLIC c_std_11)