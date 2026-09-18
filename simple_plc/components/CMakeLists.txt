# components/CMakeLists.txt - Layer 1 (SX Driver Core)
#
# Builds the chip-agnostic driver contracts + the one driver with real
# cross-platform logic today (USB CDC over TinyUSB) as a static library,
# splc_components. Per docs/architecture.md section 1, Layer 1 sits above
# Layer 0 (platforms/) -- it may declare the sx_<peripheral>.h contracts
# and implement anything that isn't chip-specific, but the actual
# HAL_*() calls live one layer down, in platforms/, not here.
#
# gpio/adc/flash/uart/time under this directory are HEADER-ONLY (0 .c
# files) -- sx_gpio.h etc. are pure contracts; stm32h5_gpio.c (Layer 0)
# is what implements them. Only usb_cdc/ has a real .c file at this layer,
# because sx_usb_tiny_init/read/write/process wrap TinyUSB's tud_*() API
# directly (already chip-agnostic -- TinyUSB itself handles the USB
# peripheral differences), unlike GPIO/ADC/Flash/UART which need a real
# HAL call per chip family.
#
# i2c/ and timer/ are 0-line placeholder headers today (see
# docs/handoff.md section 1.3) -- not referenced below; add them once they
# have real content.

# --- USB CDC (Layer 1, has a real .c) -----------------------------------
#
# sx_usb_cdc.c depends on three things, now all resolvable:
#   1. TinyUSB headers/sources (tusb.h, tusb_types.h, the tud_*()
#      implementation) -- built as the "tinyusb" target in
#      libs/CMakeLists.txt (requires the tinyusb submodule to be
#      checked out; see that file for the guard/error message if not).
#   2. usb.h (Core/Inc/usb.h, CubeMX-generated) and tusb_config.h
#      (port/usb/tusb_config.h, this project's own TinyUSB compile-time
#      config) -- usb.h comes from the stm32cubemx target (transitively,
#      via linking tinyusb below, which already links stm32cubemx);
#      tusb_config.h's directory is added to the include path below.
#   3. logger.h (Layer U, utils/logger/) -- added to the include path
#      below. Safe to use bare-metal as-is (FreeRTOS calls are already
#      guarded behind `#if FREE_RTOS`, defaulting to 0 in logger.h).
set(SPLC_COMPONENTS_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/usb_cdc/sx_usb_cdc.c
)

add_library(splc_components STATIC ${SPLC_COMPONENTS_SRC})

# PUBLIC (STATIC): platforms/stm32/stm32h5/*.c #include these sx_*.h
# contracts directly (e.g. stm32h5_gpio.c -> sx_gpio.h), and any Layer 3
# code calling sx_gpio_read()/sx_uart_write()/etc. needs them too.
set(SPLC_COMPONENTS_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/gpio
    ${CMAKE_CURRENT_SOURCE_DIR}/adc
    ${CMAKE_CURRENT_SOURCE_DIR}/flash
    ${CMAKE_CURRENT_SOURCE_DIR}/uart
    ${CMAKE_CURRENT_SOURCE_DIR}/time
    ${CMAKE_CURRENT_SOURCE_DIR}/usb_cdc
)

target_include_directories(splc_components PUBLIC ${SPLC_COMPONENTS_INCLUDE_DIRS})

# sx_uart.h (and sx_usb_cdc.c, via cqueue-backed RX/TX buffers) depend on
# cqueue.h (Layer U, utils/cqueue/). Re-exported here so nothing above
# this layer needs to know that a Layer U dependency is hiding inside a
# Layer 1 header.
target_include_directories(splc_components PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/cqueue
)

# sx_usb_cdc.c #includes "logger.h" (Layer U, utils/logger/) directly.
target_include_directories(splc_components PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/logger
)

# sx_usb_cdc.c #includes "sx_platform_config.h" (currently app/, Layer 4
# -- see the same TODO already flagged in
# platforms/stm32/stm32h5/CMakeLists.txt about this header living one
# layer too high). Needed here for the same reason: sx_usb_cdc.c's
# `#if STM32H5_PLATFORM` branch must resolve this macro to build at all.
target_include_directories(splc_components PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../app
)

# sx_usb_cdc.c #includes "tusb.h" (TinyUSB) and "usb.h" (CubeMX-generated
# Core/Inc/usb.h, transitively resolvable via stm32cubemx which the
# tinyusb target already links PUBLIC). PUBLIC: any Layer 3.5/3 code
# consuming splc_components' sx_usb_cdc.h (e.g. port/modbus_usb/
# modbus_usb.c) transitively needs these resolvable too, since
# sx_usb_cdc.h itself does not include tusb.h/usb.h but sx_usb_cdc.c does.
#
# NOTE: if the tinyusb submodule has not been checked out yet, the
# "tinyusb" target does not exist (see libs/CMakeLists.txt's EXISTS
# guard) and this line will fail CMake configuration with "target
# 'tinyusb' not found" -- a clear, early error rather than the confusing
# "tusb_types.h: No such file" compiler error this used to fail with.
target_link_libraries(splc_components PUBLIC
    tinyusb
)

target_compile_features(splc_components PUBLIC c_std_11)