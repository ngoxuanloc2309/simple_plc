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
    splc_core
    splc_components
    splc_platform_stm32h5
    nanomodbus
    splc_port
    splc_services
    splc_board
)

# --- Everything else: not yet split into its own CMakeLists.txt --------
#
# NOTE: Layer 4 (app/plc_app/plc_engine.c) is currently an EMPTY
# placeholder -- see docs/architecture.md section 10, "still open". It
# is intentionally left out of target_sources below; add it once
# implemented, or the link step will fail on missing definitions for
# symbols declared in its header.

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # TODO: Layer 4 (app/) - plc_engine.c is currently empty; add once
    #       implemented.

    # Layer U - Utils (pure algorithms, no hardware dependency)
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/cqueue/cqueue.c
    # NOTE: utils/logger/logger.c moved to board/CMakeLists.txt's
    # splc_board target -- board_<sku>.c is its only caller today. See
    # that file's comment: it builds clean bare-metal (FREE_RTOS=0
    # default), the older claim here that it "still needs the FreeRTOS
    # mutex stripped" was stale.
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # NOTE: Layer 2/1/0/3.5/libs/3 (core/, components/, platforms/,
    # port/, libs/, services/) are no longer listed here -- each exposes
    # its own headers as PUBLIC include dirs automatically via
    # target_link_libraries() above. The two include dirs below are kept
    # only for Layer 4 headers, which have no CMakeLists.txt / target of
    # their own yet.

    # Layer 4 (headers only referenced today)
    ${CMAKE_CURRENT_SOURCE_DIR}/app

    # Layer U
    ${CMAKE_CURRENT_SOURCE_DIR}/utils
)