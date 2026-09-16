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
# TODO(wiring): sx_usb_cdc.c depends on three things NOT YET WIRED
# anywhere in the CMake tree, so splc_components as defined below will
# fail to compile out of the box until each is resolved. Left as TODOs
# rather than silently working around them, since guessing wrong here
# (e.g. pointing at the wrong tinyusb config) would be worse than a clear
# build failure with a pointer to the fix:
#
#   1. TinyUSB headers/sources (tusb.h, tusb_types.h, the tud_*()
#      implementation). libs/tinyusb is a git submodule
#      (see /.gitmodules) that is NOT checked out in this working copy
#      (the directory exists but is empty) -- `git submodule update
#      --init --recursive` must be run first, and TinyUSB's own include
#      dirs + a small set of its .c files (usbd.c, cdc_device.c, the
#      dcd_* device-controller driver for STM32H5, tusb.c) need to be
#      added to a target before splc_components can actually link.
#   2. usb.c / tusb_config.h / usb_descriptors.c -- port/usb/ has
#      tusb_config.h and usb_descriptors.c (CDC-only descriptor set) but
#      port/ has no CMakeLists.txt yet; sx_usb_cdc.c's
#      `#include "usb.h"` (Core/Inc/usb.c's header, CubeMX-generated) and
#      `#include "tusb_config.h"` both need an include path added here or
#      one layer up once port/'s CMakeLists.txt exists.
#   3. logger.h (Layer U, utils/logger/) -- safe to use bare-metal as-is
#      (FreeRTOS calls are already guarded behind `#if FREE_RTOS`,
#      defaulting to 0 in logger.h), but utils/ needs to be on the
#      include path wherever splc_components is consumed.
#
# Once (1)-(3) are resolved, add usb_cdc/sx_usb_cdc.c to
# SPLC_COMPONENTS_SRC below and link this target against whatever TinyUSB
# target ends up owning tusb.h.
set(SPLC_COMPONENTS_SRC
    # usb_cdc/sx_usb_cdc.c intentionally NOT listed yet -- see TODO above.
)

if(SPLC_COMPONENTS_SRC)
    add_library(splc_components STATIC ${SPLC_COMPONENTS_SRC})
else()
    # No real .c files wired in yet (see TODO above) -- an INTERFACE
    # library still lets platforms/ link against splc_components for the
    # sx_*.h header contracts without requiring a compiled source file
    # that doesn't build yet. Switch to add_library(... STATIC ...) above
    # once sx_usb_cdc.c is added to SPLC_COMPONENTS_SRC.
    add_library(splc_components INTERFACE)
endif()

# PUBLIC (STATIC) / INTERFACE: platforms/stm32/stm32h5/*.c #include these
# sx_*.h contracts directly (e.g. stm32h5_gpio.c -> sx_gpio.h), and any
# Layer 3 code calling sx_gpio_read()/sx_uart_write()/etc. needs them too.
set(SPLC_COMPONENTS_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/gpio
    ${CMAKE_CURRENT_SOURCE_DIR}/adc
    ${CMAKE_CURRENT_SOURCE_DIR}/flash
    ${CMAKE_CURRENT_SOURCE_DIR}/uart
    ${CMAKE_CURRENT_SOURCE_DIR}/time
    ${CMAKE_CURRENT_SOURCE_DIR}/usb_cdc
)

if(SPLC_COMPONENTS_SRC)
    target_include_directories(splc_components PUBLIC ${SPLC_COMPONENTS_INCLUDE_DIRS})
else()
    target_include_directories(splc_components INTERFACE ${SPLC_COMPONENTS_INCLUDE_DIRS})
endif()

# sx_uart.h (and, once wired, sx_usb_cdc.c) depend on cqueue.h (Layer U,
# utils/cqueue/). Re-exported here so nothing above this layer needs to
# know that a Layer U dependency is hiding inside a Layer 1 header.
if(SPLC_COMPONENTS_SRC)
    target_include_directories(splc_components PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/../utils/cqueue
    )
else()
    target_include_directories(splc_components INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/../utils/cqueue
    )
endif()

if(SPLC_COMPONENTS_SRC)
    target_compile_features(splc_components PUBLIC c_std_11)
else()
    target_compile_features(splc_components INTERFACE c_std_11)
endif()