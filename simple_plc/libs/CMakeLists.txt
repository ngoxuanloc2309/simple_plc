# libs/CMakeLists.txt - Layer "Library" (third-party code)
#
# Builds vendored/submoduled third-party libraries as their own CMake
# targets, so upper layers (port/, components/) can link against them by
# name instead of reaching into libs/ with raw include paths. This is
# the layer directly below port/ (Layer 3.5) in
# docs/architecture.md section 1/3 -- port/usb/, .../modbus_serial/
# and .../modbus_tcp/ all exist to adapt what gets built here to this
# project's own Layer 1 components.

# --- nanoMODBUS (SimplePLC's private, renamed copy) -----------------------
#
# Vendored directly (not a git submodule -- unlike tinyusb below, the
# source is committed straight into libs/nanomodbus/ and is NOT modified).
# nanomodbus.c uses only <stdbool.h>/<stdint.h>/<string.h> and has no
# platform-specific code and no global data: all GPIO/UART/USB access is
# pushed out to the nmbs_platform_conf function pointers the *caller*
# supplies (see port/modbus_link, port/modbus_serial). That is exactly what
# makes it safe to keep TWO copies in one firmware, and that is what
# SimplePLC does: a host firmware that embeds SimplePLC usually has its own
# nanoMODBUS (same names, maybe another version or other compile flags), and
# sharing it would tie SimplePLC's reliability to things it does not control.
#
# libs/splc_nanomodbus/ is the wrapper that makes the two coexist:
#   - splc_nmbs.h renames every global function to splc_<name> (including the
#     recv_*_res ones, which have no nmbs_ prefix), #undef's every NMBS_*
#     compile-time option so a host's -DNMBS_SERVER_DISABLED cannot leak in,
#     and includes the vendored header by relative path;
#   - splc_nmbs.c compiles the vendored nanomodbus.c through that header.
# All SimplePLC code includes "splc_nmbs.h", never "nanomodbus.h". The target
# is named splc_nanomodbus (not "nanomodbus") so it cannot clash with a
# host's own CMake target either, and only libs/splc_nanomodbus/ is put on the
# include path: the vendored directory is deliberately NOT exported, so a
# stray #include "nanomodbus.h" cannot resolve to it and shadow the host's.
#
# This target builds identically for the real STM32H5 firmware and for a
# plain-PC test binary, same as splc_core (core/CMakeLists.txt).
add_library(splc_nanomodbus STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/splc_nanomodbus/splc_nmbs.c
)

target_include_directories(splc_nanomodbus PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/splc_nanomodbus
)

target_compile_features(splc_nanomodbus PUBLIC c_std_11)

# Fail the build if the renaming is incomplete (new nanoMODBUS version, typo):
# otherwise it would only surface later as a duplicate symbol at the host's
# link. Skipped, with a warning, when no `nm` is known to CMake.
if(CMAKE_NM)
    add_custom_command(TARGET splc_nanomodbus POST_BUILD
        COMMAND ${CMAKE_COMMAND}
                -DNM=${CMAKE_NM}
                -DLIB=$<TARGET_FILE:splc_nanomodbus>
                -P ${CMAKE_CURRENT_SOURCE_DIR}/../cmake_modules/splc_check_symbols.cmake
        COMMENT "Checking splc_nanomodbus exports only splc_-prefixed symbols"
        VERBATIM
    )
else()
    message(WARNING "CMAKE_NM not set: splc_nanomodbus symbol-prefix check skipped")
endif()

# --- TinyUSB (optional: SPLC_PORT_USB_CDC = 1) ---------------------------
#
# TinyUSB is part of the optional USB-CDC port module (port/usb/), not of the
# core. With SPLC_PORT_USB_CDC = 0 (splcopts.h, default) nothing below is
# built: the host firmware owns the USB peripheral and its own TinyUSB, and
# SimplePLC must not bring a second copy (USB peripheral, interrupt and
# descriptors can have only ONE owner -- docs/architecture.md section 3.1).
#
# libs/tinyusb is a git submodule (see /.gitmodules: path
# simple_plc/libs/tinyusb, url https://github.com/hathach/tinyusb.git).
# `git submodule update --init --recursive` must be run from the repo
# root before there is anything here to build. With the module enabled and
# the submodule missing, configure STOPS here with a clear message instead of
# failing later with "tusb.h: No such file".
#
# Driver choice -- stm32_fsdev, NOT dwc2/synopsys: TinyUSB's own
# manufacturer support table (README.md, "ST STM32" row) and its
# hw/bsp/stm32h5/ Nucleo board port both confirm the H5 family's USB
# peripheral (USB_DRD_FS on the .ioc / RM0481 naming -- see
# RS485_IO_RF_V2.ioc's Mcu.IP21=USB and NVIC.USB_DRD_FS_IRQn) is the same
# "full-speed device-only" peripheral family as C0/G0/U3, handled by
# src/portable/st/stm32_fsdev/, NOT the OTG/dwc2 peripheral used on
# F2/F4/F7/H7.
if(SPLC_PORT_USB_CDC)
    if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tinyusb/src/tusb.c)
        message(FATAL_ERROR
            "SimplePLC: SPLC_PORT_USB_CDC = 1 (splcopts.h) needs TinyUSB, but the "
            "submodule libs/tinyusb is not checked out.\n"
            "Run:  git submodule update --init --recursive\n"
            "or set SPLC_PORT_USB_CDC to 0 if the host firmware owns the USB peripheral.")
    endif()

    # tinyusb_target_add() is TinyUSB's own official helper (defined in
    # libs/tinyusb/src/CMakeLists.txt -- vendored, not ours to edit): it
    # adds every common+device+host+typec .c file plus TinyUSB's own
    # include dirs to whatever target name is passed in. DCD/HCD
    # (device/host controller driver) sources are deliberately NOT
    # included by tinyusb_sources_get() (see that file's own comment) --
    # exactly one dcd_*.c matching this MCU's USB peripheral must be
    # added separately, which is what the file(GLOB ...) below is for.
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/tinyusb/src)

    add_library(tinyusb STATIC)

    tinyusb_target_add(tinyusb)

    # This project's own CDC-only descriptor set (port/usb/usb_descriptors.c,
    # port/usb/tusb_config.h) plus the stm32_fsdev DCD/HCD driver sources
    # for this MCU family (src/portable/st/stm32_fsdev/*.c -- dcd_stm32_fsdev.c,
    # hcd_stm32_fsdev.c, fsdev_common.c; HCD/host isn't used on this
    # device-only product but building it costs nothing extra and avoids
    # hand-picking files out of the glob).
    file(GLOB TINYUSB_PORT_SRC
        "${CMAKE_CURRENT_SOURCE_DIR}/../port/usb/usb_descriptors.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/tinyusb/src/portable/st/stm32_fsdev/*.c"
    )

    target_sources(tinyusb PRIVATE ${TINYUSB_PORT_SRC})

    target_include_directories(tinyusb PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/../port/usb
        ${CMAKE_CURRENT_SOURCE_DIR}/tinyusb/src/common
        ${CMAKE_CURRENT_SOURCE_DIR}/tinyusb/src/device
        ${CMAKE_CURRENT_SOURCE_DIR}/tinyusb/src/portable/st/stm32_fsdev
    )

    # stm32cubemx (INTERFACE target, cmake/stm32cubemx/CMakeLists.txt,
    # added via add_subdirectory() before this file per root
    # CMakeLists.txt) supplies stm32h5xx_hal.h, USE_HAL_DRIVER/STM32H523xx
    # compile definitions, and Core/Inc (for usb.h, the CubeMX-generated
    # USB peripheral init header dcd_stm32_fsdev.c needs). PUBLIC so anything
    # linking "tinyusb" transitively resolves these too.
    target_link_libraries(tinyusb PUBLIC
        stm32cubemx
    )

    target_compile_features(tinyusb PUBLIC c_std_11)

    message(STATUS "libs/CMakeLists.txt: SPLC_PORT_USB_CDC=1 -- 'tinyusb' target built (stm32_fsdev driver, this project's port/usb/ descriptors).")
else()
    message(STATUS "libs/CMakeLists.txt: SPLC_PORT_USB_CDC=0 -- TinyUSB not built (host firmware owns USB).")
endif()