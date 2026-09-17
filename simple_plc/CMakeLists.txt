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
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE splc_core)

# --- Layer 1 - SX Driver Core (components/) -----------------------------
# splc_components is defined in components/CMakeLists.txt. Currently an
# INTERFACE library (no compiled .c files yet -- its one real source,
# usb_cdc/sx_usb_cdc.c, is deliberately left out until the tinyusb
# submodule is checked out and wired; see the TODO in
# components/CMakeLists.txt). Still safe to add_subdirectory() and link
# now: an INTERFACE target contributes only include dirs/compile
# features, no object code, so this line will keep working unchanged
# once sx_usb_cdc.c is added there and the target becomes STATIC.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/components)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE splc_components)

# --- Layer 0 - Platform (platforms/stm32/stm32h5/) -----------------------
# splc_platform_stm32h5 is defined in
# platforms/stm32/stm32h5/CMakeLists.txt. Builds gpio/flash/uart/time
# today. NOTE: stm32h5_adc.c is included in that target's source list but
# will FAIL TO COMPILE as of this writing -- it #includes "adc.h", a
# CubeMX-generated header that does not exist yet because ADC is not
# configured in RS485_IO_RF_V2.ioc (see the VERIFIED BUILD GAP comment in
# platforms/stm32/stm32h5/CMakeLists.txt for the full explanation). This
# add_subdirectory() will therefore make the very first real build fail
# on adc.h until someone opens the .ioc in STM32CubeMX, configures the
# ADC instance/channels for the 4AI SKU, and regenerates code. Left in
# rather than worked around, so that failure is loud and points at the
# right fix instead of silently shipping firmware with AI reads missing.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/platforms/stm32/stm32h5)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE splc_platform_stm32h5)

# --- Library - nanoMODBUS (libs/) ---------------------------------------
# nanomodbus is defined in libs/CMakeLists.txt. Portable, builds cleanly
# standalone (verified independently), no platform dependency.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/libs)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE nanomodbus)

# --- Layer 3.5 - Protocol / Library Porting (port/) ---------------------
# splc_port is defined in port/CMakeLists.txt. Builds
# port/modbus_usb/modbus_usb.c today (the App<->MCU channel over
# USB-CDC); port/modbus_serial/ and port/modbus_tcp/ remain stubs and are
# deliberately excluded there (see the NOTE blocks in port/CMakeLists.txt
# -- Gateway-variant transports, out of scope for the current Remote I/O
# SKU per docs/architecture.md).
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/port)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE splc_port)

# --- Everything else: not yet split into its own CMakeLists.txt --------
#
# NOTE: Layer 3 (services/) and Layer 4 (app/) .c files exist but are
# currently EMPTY placeholders (plc_engine.c, plc_io.c, plc_modbus_cfg.c,
# plc_retain.c) -- see docs/architecture.md section 10, "still open".
# They are intentionally left out of target_sources below; add them back
# in as each is implemented, or the link step will fail on missing
# definitions for symbols declared in their headers.

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # TODO: Layer 3 (services/) - plc_io.c, plc_modbus_cfg.c, plc_retain.c
    #       are currently empty; add once implemented. plc_retain.c is the
    #       best starting point -- it only needs Layer 0's flash driver
    #       (already wired above), not a pin map or USB.
    # TODO: board/board.c is currently EMPTY (0 lines) -- left out of
    #       target_sources on purpose; add once it contains real
    #       definitions, or the executable will still link fine but
    #       silently do nothing useful.
    # TODO: Layer 4 (app/) - plc_engine.c is currently empty; add once
    #       implemented.

    # Layer U - Utils (pure algorithms, no hardware dependency)
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/cqueue/cqueue.c
    # TODO: utils/logger/logger.c still #includes <FreeRTOS.h>/<semphr.h>
    # and calls xSemaphoreTake/Give/CreateMutex. docs/architecture.md
    # section "Layer U" says this mutex must be stripped when porting from
    # WS_v1, since SimplePLC runs bare-metal single-threaded with no
    # FreeRTOS. Left out of target_sources until that port is done, or
    # this will fail to compile/link without FreeRTOS present.
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # NOTE: Layer 2 (core/plc_tag, core/plc_rule) is no longer listed here
    # -- splc_core exposes those as PUBLIC include dirs automatically via
    # target_link_libraries() above. Same is now true for Layer 0/1/3.5/
    # libs (splc_platform_stm32h5, splc_components, splc_port,
    # nanomodbus) via their own target_link_libraries() calls above --
    # the four include dirs below are kept only for Layer 3/4 headers,
    # which have no CMakeLists.txt / target of their own yet.

    # Layer 3 / 4 (headers only referenced today)
    ${CMAKE_CURRENT_SOURCE_DIR}/services
    ${CMAKE_CURRENT_SOURCE_DIR}/app

    # Layer U
    ${CMAKE_CURRENT_SOURCE_DIR}/utils
)