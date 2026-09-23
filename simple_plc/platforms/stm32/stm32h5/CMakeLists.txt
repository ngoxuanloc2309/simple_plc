# platforms/stm32/stm32h5/CMakeLists.txt - Layer 0 (Platform)
#
# Builds the STM32H5-specific driver implementations as a static library,
# splc_platform_stm32h5. Per docs/architecture.md section 1, Layer 0 is the
# ONLY layer allowed to include vendor HAL headers -- this target is where
# that boundary lives in the build.
#
# Each stm32h5_<peripheral>.c file implements the chip-agnostic contract
# declared in components/<peripheral>/sx_<peripheral>.h (Layer 1), so this
# target depends on splc_components (see components/CMakeLists.txt) for
# those headers, and on the stm32cubemx INTERFACE target (see
# cmake/stm32cubemx/CMakeLists.txt) for the real HAL headers
# (stm32h5xx_hal.h, stm32h523xx.h, ...) and the USE_HAL_DRIVER/STM32H523xx
# compile definitions every stm32h5_*.h file's "#if STM32H5_PLATFORM"
# branch pulls in.
#
# NOTE on sx_platform_config.h: every stm32h5_*.h in this tree does
# `#include "sx_platform_config.h"` before checking `#if STM32H5_PLATFORM`.
# That header currently lives in app/ (Layer 4), not in this layer -- see
# the TODO below. It must resolve for this target to build at all.
#
# Timer (stm32h5_timer.c/.h) and I2C are 0 lines today (see
# docs/handoff.md section 1.3) -- deliberately left out of
# SPLC_PLATFORM_STM32H5_SRC below; add stm32h5_timer.c once it has real
# content, or the build will link fine but silently be missing timer
# support.
#
# VERIFIED BUILD GAP (found by actually compiling each .c file with the
# include paths this CMakeLists declares, not just by reading code):
# stm32h5_adc.c does `#include "adc.h"`, CubeMX's generated ADC handle
# header (normally Core/Inc/adc.h + Core/Src/adc.c, declaring hadc1 etc.).
# That file DOES NOT EXIST in this repo -- RS485_IO_RF_V2.ioc has no ADC
# peripheral configured at all yet (confirmed: no "ADC" section in the
# .ioc, and Core/Inc has no adc.h). stm32h5_adc.c was written against a
# CubeMX-generated file that has never actually been generated. Every
# other file in SPLC_PLATFORM_STM32H5_SRC below (gpio, flash, uart, time)
# compiles cleanly against Core/Inc as it exists today; only adc does not.
# Someone needs to open the .ioc in STM32CubeMX, enable the ADC instance/
# channel(s) the 4AI Remote I/O SKU needs, and regenerate code (which
# creates Core/Inc/adc.h + Core/Src/adc.c and adds them to
# cmake/stm32cubemx/CMakeLists.txt's MX_Application_Src automatically)
# before stm32h5_adc.c can build. Left in the source list below rather
# than commented out, so the build fails loudly with a clear
# "adc.h: No such file" instead of silently shipping firmware with AI
# reads unimplemented.

set(SPLC_PLATFORM_STM32H5_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/gpio/stm32h5_gpio.c
    ${CMAKE_CURRENT_SOURCE_DIR}/adc/stm32h5_adc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/flash/stm32h5_flash.c
    ${CMAKE_CURRENT_SOURCE_DIR}/uart/stm32h5_uart.c
    ${CMAKE_CURRENT_SOURCE_DIR}/time/stm32h5_time.c
    ${CMAKE_CURRENT_SOURCE_DIR}/system/stm32h5_system.c
)

add_library(splc_platform_stm32h5 STATIC ${SPLC_PLATFORM_STM32H5_SRC})

# PUBLIC: any Layer 1/3/4 target that links splc_platform_stm32h5 also needs
# to resolve stm32h5_gpio.h etc. (e.g. board init code choosing which pin
# maps to which sx_gpio_pin_t). Mirrors the PUBLIC pattern already used by
# splc_core in core/CMakeLists.txt for its own headers.
target_include_directories(splc_platform_stm32h5 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/gpio
    ${CMAKE_CURRENT_SOURCE_DIR}/adc
    ${CMAKE_CURRENT_SOURCE_DIR}/flash
    ${CMAKE_CURRENT_SOURCE_DIR}/uart
    ${CMAKE_CURRENT_SOURCE_DIR}/time
    ${CMAKE_CURRENT_SOURCE_DIR}/timer
    ${CMAKE_CURRENT_SOURCE_DIR}/flash_define
    ${CMAKE_CURRENT_SOURCE_DIR}/pwd
    ${CMAKE_CURRENT_SOURCE_DIR}/system
)

# TODO(wiring): sx_platform_config.h (the STM32H5_PLATFORM=1 / *_PLATFORM=0
# switch every header here #includes) currently lives under simple_plc/app/,
# not under this layer. Layer 0 including a Layer 4 header is exactly the
# "include runs one direction, top to bottom" rule in
# docs/architecture.md section 1 being violated in spirit, even though it
# compiles today because CMake include paths don't enforce layering by
# themselves. Left in PUBLIC here (not PRIVATE) only so the current file
# layout keeps building; whoever wires this into simple_plc/CMakeLists.txt
# should flag this to move sx_platform_config.h down into this layer (or a
# new Layer "board select" one level below app/) instead of leaving the
# dependency pointed upward.
target_include_directories(splc_platform_stm32h5 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../app
)

# splc_components (Layer 1) supplies the sx_gpio.h / sx_adc.h / sx_flash.h /
# sx_uart.h / sx_time.h contracts each stm32h5_*.c implements, plus cqueue.h
# (Layer U, re-exported PUBLIC by splc_components) that sx_uart.h depends on.
# PUBLIC so a caller that only links splc_platform_stm32h5 without also
# explicitly linking splc_components still resolves those headers -- Layer 0
# .c files #include them directly (e.g. stm32h5_gpio.c -> sx_gpio.h).
target_link_libraries(splc_platform_stm32h5 PUBLIC
    splc_components
)

# stm32cubemx (INTERFACE target, cmake/stm32cubemx/CMakeLists.txt) supplies
# stm32h5xx_hal.h / stm32h523xx.h and the USE_HAL_DRIVER / STM32H523xx
# compile definitions. PUBLIC: this is a vendor HAL dependency genuinely
# needed by anyone consuming this Layer 0 library's headers too (e.g.
# stm32h5_gpio.h itself #includes stm32h5xx_hal.h), not just by this
# target's own .c files.
#
# NOTE: this target name comes from the existing
# cmake/stm32cubemx/CMakeLists.txt at the repo root and must already be
# added via add_subdirectory() by the time this file is processed -- see
# root CMakeLists.txt, which does add_subdirectory(cmake/stm32cubemx)
# BEFORE add_subdirectory(simple_plc).
target_link_libraries(splc_platform_stm32h5 PUBLIC
    stm32cubemx
)

target_compile_features(splc_platform_stm32h5 PUBLIC c_std_11)