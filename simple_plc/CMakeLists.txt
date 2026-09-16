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

# --- Everything else: not yet split into its own CMakeLists.txt --------
#
# NOTE: Layer 3 (services/) and Layer 4 (app/) .c files exist but are
# currently EMPTY placeholders (plc_engine.c, plc_io.c, plc_modbus_cfg.c,
# plc_retain.c) -- see docs/architecture.md section 10, "still open".
# They are intentionally left out of target_sources below; add them back
# in as each is implemented, or the link step will fail on missing
# definitions for symbols declared in their headers.

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # TODO: Layer 0 (platforms/) - add stm32h5_gpio.c, stm32h5_uart.c,
    # stm32h5_timer.c once wired to a board pin map.
    # TODO: Layer 1 (components/) - SX Driver Core sources, once present.
    # TODO: Layer 3 (services/) - plc_io.c, plc_modbus_cfg.c, plc_retain.c
    #       are currently empty; add once implemented.
    # TODO: Layer 3.5 (port/) - port/modbus_serial/modbus_serial.c only
    #       has #include lines today (no real logic yet); port/usb/
    #       (modbus_usb, the actual App<->MCU channel per v1.7) has not
    #       been started. Neither is added to target_sources yet.
    # TODO: board/board.c and port/usb/usb_descriptors.c are currently
    #       EMPTY (0 lines) -- left out of target_sources on purpose;
    #       add once they contain real definitions, or the executable
    #       will still link fine but silently do nothing useful.
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
    # target_link_libraries() above.

    # Layer 3 / 3.5 / 4 (headers only referenced today)
    ${CMAKE_CURRENT_SOURCE_DIR}/services
    ${CMAKE_CURRENT_SOURCE_DIR}/port
    ${CMAKE_CURRENT_SOURCE_DIR}/app

    # Layer 1 / 0
    ${CMAKE_CURRENT_SOURCE_DIR}/components

    # Layer U
    ${CMAKE_CURRENT_SOURCE_DIR}/utils

    # Third-party (nanoMODBUS submodule, etc.)
    ${CMAKE_CURRENT_SOURCE_DIR}/libs
)