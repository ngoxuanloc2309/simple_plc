# components/CMakeLists.txt - Layer 1 (SX Driver Core)
#
# The chip-agnostic driver CONTRACTS (sx_gpio.h, sx_uart.h, sx_flash.h, ...) as
# a header-only INTERFACE library, splc_components. Per docs/architecture.md
# section 1, Layer 1 sits above Layer 0 (platforms/) -- it declares the
# sx_<peripheral>.h contracts; the actual HAL_*() calls live one layer down,
# in platforms/, not here.
#
# There is NO .c file at this layer: the USB-CDC transport is the optional
# port module port/usb/ (only with SPLC_PORT_USB_CDC = 1), not a Layer 1 driver.
# Nothing here depends on TinyUSB.
#
# i2c/ and timer/ are 0-line placeholder headers today -- not referenced
# below; add them once they have real content.

add_library(splc_components INTERFACE)

# INTERFACE: platforms/stm32/stm32h5/*.c #include these sx_*.h contracts
# directly (e.g. stm32h5_gpio.c -> sx_gpio.h), and any Layer 3 code calling
# sx_gpio_read()/sx_uart_write()/etc. needs them too.
target_include_directories(splc_components INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/gpio
    ${CMAKE_CURRENT_SOURCE_DIR}/adc
    ${CMAKE_CURRENT_SOURCE_DIR}/flash
    ${CMAKE_CURRENT_SOURCE_DIR}/uart
    ${CMAKE_CURRENT_SOURCE_DIR}/time
    ${CMAKE_CURRENT_SOURCE_DIR}/os
    ${CMAKE_CURRENT_SOURCE_DIR}/system
    ${CMAKE_CURRENT_SOURCE_DIR}/rtc
    ${CMAKE_CURRENT_SOURCE_DIR}/pwd
)

# sx_os.h (components/os/) is only the contract; its FreeRTOS implementation
# and the RTOS include dirs (SPLC_OS_INCLUDE_DIRS) live in Layer 0:
# platforms/freertos/CMakeLists.txt.

# sx_uart.h depends on splc_cqueue.h (Layer U, utils/cqueue/). Re-exported here so
# nothing above this layer needs to know that a Layer U dependency is hiding
# inside a Layer 1 header.
target_link_libraries(splc_components INTERFACE splc_utils)

# Layer 1 / Layer 0 code logs through config/splc_log.h, which includes
# "logger.h". Built-in logger: utils/logger. SPLC_LOGGER_BUILTIN=0: the host
# firmware supplies logger.h and this directory is NOT added (same file name
# as the host's).
if(SPLC_LOGGER_INCLUDE_DIR)
    target_include_directories(splc_components INTERFACE ${SPLC_LOGGER_INCLUDE_DIR})
endif()

# "sx_platform_config.h" / "sx_os_config.h" currently live in app/ (Layer 4 --
# the same TODO already flagged in platforms/stm32/stm32h5/CMakeLists.txt about
# these headers living one layer too high); Layer 1 users (sx_os.h) need them.
target_include_directories(splc_components INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/../app
)

target_compile_features(splc_components INTERFACE c_std_11)