# simple_plc/CMakeLists.txt
#
# Wires the layered simple_plc/ library into the parent STM32 firmware
# target (RS485_IO_RF_V2, defined in the root CMakeLists.txt).
#
# Layering approach: each layer that has real, compiling sources gets its
# own CMakeLists.txt exposing a small static library target (e.g.
# core/CMakeLists.txt -> splc_core for Layer 2). This is what makes it
# possible to link only Layer 2 into a plain-PC unit test binary later,
# without pulling in the STM32 HAL. Layers that are still empty
# placeholders (see below) are not yet split out; they are added directly
# to the executable for now and will get their own CMakeLists.txt as they
# gain real content.

# --- Product configuration (splcopts.h) --------------------------------
# Same model as lwIP's lwipopts.h. The library supplies the defaults
# (config/splc_opt.h); the PRODUCT supplies splcopts.h and says where it is:
#     cmake ... -DSPLC_OPTS_DIR=<dir containing splcopts.h>
# Default: <top-level source dir>/splc_config. Users configure the library by
# editing that one file, never by editing anything under simple_plc/.
# Two mechanisms on purpose: include_directories() (directory scope) gives
# every layer target created below both dirs without touching each of their
# CMakeLists; the INTERFACE target splc_config is linked into the top-level
# executable (its own sources, e.g. main.c, include plc_engine.h ->
# splc_opt.h, and directory scope does not reach targets of the parent).
set(SPLC_OPTS_DIR "${CMAKE_SOURCE_DIR}/splc_config" CACHE PATH
    "Directory containing the product's splcopts.h (see config/splcopts_template.h)")
if(NOT EXISTS "${SPLC_OPTS_DIR}/splcopts.h")
    message(FATAL_ERROR
        "SimplePLC: splcopts.h not found in SPLC_OPTS_DIR='${SPLC_OPTS_DIR}'.\n"
        "Copy ${CMAKE_CURRENT_SOURCE_DIR}/config/splcopts_template.h to that directory "
        "as splcopts.h, or pass -DSPLC_OPTS_DIR=<dir>.")
endif()
message(STATUS "SimplePLC options: ${SPLC_OPTS_DIR}/splcopts.h")
include_directories(
    ${SPLC_OPTS_DIR}                          # product's splcopts.h
    ${CMAKE_CURRENT_SOURCE_DIR}/config        # library's splc_opt.h
)
add_library(splc_config INTERFACE)
target_include_directories(splc_config INTERFACE
    ${SPLC_OPTS_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/config
)
# Rebuild/re-configure when the options file changes.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SPLC_OPTS_DIR}/splcopts.h")

# --- Layer 2 - PLC Core -----------------------------------------------
# splc_core is defined in core/CMakeLists.txt. It has no dependency on any
# Layer 0/1 target by design (build/port boundary, see
# docs/architecture.md section 1, rule 3).
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/core)

# --- Layer 1 - SX Driver Core (components/) -----------------------------
# splc_components is defined in components/CMakeLists.txt. STATIC library:
# builds usb_cdc/sx_usb_cdc.c, now wired against the "tinyusb" target
# (libs/CMakeLists.txt) once the tinyusb submodule is checked out.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/components)

# --- Layer 0 - Platform (platforms/stm32/stm32h5/) -----------------------
# splc_platform_stm32h5 is defined in
# platforms/stm32/stm32h5/CMakeLists.txt. Builds all 5 real sources today
# (gpio/adc/flash/uart/time) -- ADC's Core/Inc/adc.h gap (see the old
# comment this replaces) is closed: ADC1/IN1 is now configured in
# RS485_IO_RF_V2.ioc and regenerated, and two real bugs that only surfaced
# once stm32h5_adc.c actually got compiled for the first time have been
# fixed (missing #include "sx_adc.h" in stm32h5_adc.h; and
# ADC_SAMPLETIME_COMMON_1, a macro that never existed in the real STM32H5
# HAL, replaced with ADC_SAMPLETIME_247CYCLES_5 -- see docs/handoff.md
# section 1.3 for the full story and docs/handoff.md section 3 for the
# still-open question of whether 247.5 cycles is the right value for this
# product's actual analog input sources). Verified by direct gcc compile
# of all 5 .c files against this exact include path, 0 errors.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/platforms/stm32/stm32h5)

# --- Layer 0 - Platform: OS (platforms/freertos/) ------------------------
# splc_platform_freertos (an OBJECT library, see below at the executable's
# target_sources) is defined in platforms/freertos/CMakeLists.txt.
# FreeRTOS implementation of components/os/sx_os.h; empty object unless
# app/sx_os_config.h sets SX_OS_USE_FREERTOS = 1. Also owns the cache variable
# SPLC_OS_INCLUDE_DIRS, so it must come before board/ (which reads it).
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/platforms/freertos)

# --- Library - nanoMODBUS (libs/) ---------------------------------------
# nanomodbus is defined in libs/CMakeLists.txt. Portable, builds cleanly
# standalone (verified independently), no platform dependency.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/libs)

# --- Layer 3.5 - Protocol / Library Porting (port/) ---------------------
# splc_port is defined in port/CMakeLists.txt. Builds
# port/modbus_usb/modbus_usb.c today (the App<->MCU channel over
# USB-CDC); port/modbus_serial/ and port/modbus_tcp/ remain stubs and are
# deliberately excluded there (see the NOTE blocks in port/CMakeLists.txt
# -- Gateway-variant transports, out of scope for the current Remote I/O
# SKU per docs/architecture.md).
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/port)

# --- Layer 3 - PLC Application Services (services/) ---------------------
# splc_services is defined in services/CMakeLists.txt. Builds
# plc_io.c and plc_retain.c today -- both written and verified this
# session (plc_io.c: compiles clean; plc_retain.c: compiles clean AND
# passed 5 behavioral test cases against a fake-but-flash-semantics-
# accurate backend, including sector rotation when a sector fills up --
# see docs/handoff.md for the full verification). plc_modbus_cfg.c
# remains unwritten (0 bytes) and is deliberately excluded from
# services/CMakeLists.txt's source list until it exists.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/services)

# --- Layer 4 - Board bring-up (board/) -----------------------------------
# splc_board is defined in board/CMakeLists.txt. Builds board.c +
# board_<SPLC_BOARD_SKU>.c (default "zigbee_io") -- board_hw_init()'s
# DI/DO registration + UART log + USB CDC init for that SKU. Also builds
# utils/logger/logger.c here (see board/CMakeLists.txt's own comment on
# why: it compiles clean bare-metal, the FreeRTOS mutex code is already
# guarded out via #if FREE_RTOS).
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/board)

# --- Layer 4 - Engine & Application entry (app/) -------------------------
# splc_app is defined in app/CMakeLists.txt. Builds
# app/plc_app/plc_engine.c -- plc_engine_init()/plc_engine_scan_once(),
# the single SKU-agnostic entry point Core/Src/main.c calls (see
# main.c's USER CODE BEGIN 2 / USER CODE BEGIN 3 blocks).
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/app)

# --- Link everything into the top-level executable ----------------------
#
# IMPORTANT: this must be a single PLAIN-signature target_link_libraries()
# call, not one PRIVATE-keyword call per library as this file used to
# have. Root cause (found by actually building with the real ARM
# toolchain on Windows -- this sandbox's own verification used a
# hand-written INTERFACE stand-in for the stm32cubemx target and never
# caught it): cmake/stm32cubemx/CMakeLists.txt (CubeMX-generated, not
# ours to edit -- see its own "generated only once" header comment) does
# `target_link_libraries(${CMAKE_PROJECT_NAME} ${MX_LINK_LIBS})` with NO
# PRIVATE/PUBLIC/INTERFACE keyword -- the "plain" signature. CMake
# forbids mixing plain and keyword target_link_libraries() calls on the
# same target (RS485_IO_RF_V2 here): once any call on a target is plain,
# every other call on that same target must also be plain, or configure
# fails with "All uses of target_link_libraries with a target must be
# either all-keyword or all-plain." Since cmake/stm32cubemx/CMakeLists.txt
# runs first (root CMakeLists.txt's add_subdirectory(cmake/stm32cubemx)
# comes before add_subdirectory(simple_plc)) and cannot be changed, this
# file must use plain signature too -- PRIVATE/PUBLIC only make sense
# between static libraries anyway; for the final top-level executable
# they have no real effect beyond satisfying CMake's consistency check.
target_link_libraries(${CMAKE_PROJECT_NAME}
    splc_config
    splc_core
    splc_components
    splc_platform_stm32h5
    nanomodbus
    splc_port
    splc_services
    splc_board
    splc_app
)

# sx_os_*() implementation (Layer 0, platforms/freertos/): added as OBJECT files,
# not as a static library, so it resolves for every caller regardless of the
# static-library order on the link line. Empty unless SX_OS_USE_FREERTOS = 1.
target_sources(${CMAKE_PROJECT_NAME} PRIVATE $<TARGET_OBJECTS:splc_platform_freertos>)

# RTC: keep the time across warm resets without editing CubeMX code.
# The generated MX_RTC_Init() unconditionally writes 2000-01-01 at every
# boot; these options route HAL_RTC_SetTime/SetDate through the
# __wrap_ filters in platforms/stm32/stm32h5/rtc/stm32h5_rtc.c, which skip
# that write while the clock is synced. See the comment there. (Separate
# from the target_link_libraries() call above, so the plain-vs-keyword
# rule discussed there does not apply.)
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE
    -Wl,--wrap=HAL_RTC_SetTime
    -Wl,--wrap=HAL_RTC_SetDate
)

# --- Everything else: not yet split into its own CMakeLists.txt --------

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Layer U - Utils (pure algorithms, no hardware dependency)
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/cqueue/cqueue.c
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/epoch/splc_epoch.c
    # NOTE: utils/logger/logger.c is built by board/CMakeLists.txt's
    # splc_board target instead -- board_<sku>.c is its only caller
    # today. See that file's comment.
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # NOTE: every layer (core/, components/, platforms/, port/, libs/,
    # services/, board/, app/) now exposes its own headers as PUBLIC
    # include dirs automatically via target_link_libraries() above.
    # Only Layer U (utils/cqueue/cqueue.c above) still has no
    # CMakeLists.txt of its own, hence the one include dir left here.
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/cqueue
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/epoch
)