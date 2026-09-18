# board/CMakeLists.txt - Layer 4 (board bring-up)
#
# Builds board_init()/board_hw_init() as a static library target:
# splc_board. Only ONE SKU's board_<name>.c is compiled in at a time --
# selected here via SPLC_BOARD_SKU (default "zigbee_io"), not via
# #ifdef inside a shared file (see docs/handoff.md section 1.11's
# rationale against #ifdef-based SKU selection).
#
# NOTE: utils/logger/logger.c is included directly in this target's
# sources rather than getting its own utils/CMakeLists.txt target yet --
# board_<sku>.c is currently its only caller. Verified by direct gcc
# compile with FREE_RTOS unset (defaults to 0 per logger.h): 0 errors,
# only 2 harmless warnings (printf format-security, sign-compare) --
# the #include <FreeRTOS.h>/xSemaphore* calls are already guarded inside
# #if FREE_RTOS and never reached when building bare-metal. The root
# CMakeLists.txt's older comment claiming this file "still needs the
# FreeRTOS mutex stripped" is stale; no code change was needed here.

set(SPLC_BOARD_SKU "zigbee_io" CACHE STRING "Which board SKU's board_<sku>.c to build (e.g. zigbee_io, remoteio)")

set(SPLC_BOARD_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/board.c
    ${CMAKE_CURRENT_SOURCE_DIR}/board_${SPLC_BOARD_SKU}.c
    ${CMAKE_SOURCE_DIR}/utils/logger/logger.c
)

add_library(splc_board STATIC ${SPLC_BOARD_SRC})

target_include_directories(splc_board PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_include_directories(splc_board PRIVATE
    ${CMAKE_SOURCE_DIR}/utils/logger
)

# splc_services (Layer 3) supplies plc_io.h (plc_io_register_di/do/ai()),
# which board_${SPLC_BOARD_SKU}.c calls directly.
target_link_libraries(splc_board PUBLIC
    splc_services
)

# splc_components (Layer 1) supplies sx_gpio.h/sx_uart.h/sx_usb_cdc.h,
# which board_${SPLC_BOARD_SKU}.c includes directly.
target_link_libraries(splc_board PUBLIC
    splc_components
)

# splc_core (Layer 2) supplies plc_tag_def.h (TAG_DI0/TAG_DO0 etc.),
# which board_${SPLC_BOARD_SKU}.c uses directly.
target_link_libraries(splc_board PUBLIC
    splc_core
)

target_compile_features(splc_board PUBLIC c_std_11)