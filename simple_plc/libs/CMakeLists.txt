# libs/CMakeLists.txt - Layer "Library" (third-party code)
#
# Builds vendored/submoduled third-party libraries as their own CMake
# targets, so upper layers (port/, components/) can link against them by
# name instead of reaching into libs/ with raw include paths. This is
# the layer directly below port/ (Layer 3.5) in
# docs/architecture.md section 1/3 -- port/modbus_usb/, .../modbus_serial/
# and .../modbus_tcp/ all exist to adapt what gets built here to this
# project's own Layer 1 components.

# --- nanoMODBUS -----------------------------------------------------------
#
# Vendored directly (not a git submodule -- unlike tinyusb below, the
# source is committed straight into libs/nanomodbus/), so it is always
# present. nanomodbus.c (~2460 lines) uses only <stdbool.h>/<stdint.h>/
# <string.h> and has no platform-specific code of its own -- correctness
# verified by inspection of its top #include block; it is nanoMODBUS's
# whole design that all of GPIO/UART/USB access is pushed out to the
# nmbs_platform_conf function pointers the *caller* supplies (see
# port/modbus_usb/modbus_usb.c for this project's USB-CDC one), not
# anything nanoMODBUS itself needs to know about the MCU it's running on.
# That means this target builds identically for the real STM32H5 firmware
# and for a plain-PC unit test binary, same as splc_core
# (core/CMakeLists.txt).
#
# None of nanoMODBUS's NMBS_*_DISABLED / NMBS_BITFIELD_MAX / etc. compile-
# time config macros are defined here -- every one of them is guarded by
# #ifndef in nanomodbus.h with a workable default (e.g. server AND client
# code both included, NMBS_BITFIELD_MAX 2000), so no PUBLIC compile
# definitions are needed for a first working build. If a future memory
# budget needs e.g. NMBS_SERVER_DISABLED (this device is Modbus Slave/
# Server only, per docs/architecture.md's App<->MCU channel -- it never
# acts as a Modbus Client on that link), add it via
# target_compile_definitions(nanomodbus PUBLIC NMBS_SERVER_DISABLED)
# rather than editing nanomodbus.h directly, to keep the vendored file
# unmodified.
add_library(nanomodbus STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/nanomodbus/nanomodbus.c
)

# PUBLIC: any caller (port/modbus_usb/modbus_usb.c today; eventually
# services/plc_modbus_cfg.c, Layer 3, which actually calls
# nmbs_client_create()/nmbs_server_create()) needs nanomodbus.h
# resolvable too.
target_include_directories(nanomodbus PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/nanomodbus
)

target_compile_features(nanomodbus PUBLIC c_std_11)

# --- TinyUSB ----------------------------------------------------------
#
# libs/tinyusb is a git submodule (see /.gitmodules: path
# simple_plc/libs/tinyusb, url https://github.com/hathach/tinyusb.git).
# `git submodule update --init --recursive` must be run from the repo
# root before there is anything here to build.
#
# Driver choice -- stm32_fsdev, NOT dwc2/synopsys: TinyUSB's own
# manufacturer support table (README.md, "ST STM32" row) and its
# hw/bsp/stm32h5/ Nucleo board port both confirm the H5 family's USB
# peripheral (USB_DRD_FS on the .ioc / RM0481 naming -- see
# RS485_IO_RF_V2.ioc's Mcu.IP21=USB and NVIC.USB_DRD_FS_IRQn) is the same
# "full-speed device-only" peripheral family as C0/G0/U3, handled by
# src/portable/st/stm32_fsdev/, NOT the OTG/dwc2 peripheral used on
# F2/F4/F7/H7. An earlier pass over this codebase (docs/architecture.md's
# now-superseded note) assumed dwc2/dcd_synopsys.c was needed for H5;
# that was wrong and is corrected here.
#
# The EXISTS guard means: if the submodule hasn't been fetched yet,
# `cmake configure` still succeeds (skips TinyUSB) instead of failing
# with a confusing "source file does not exist" error -- but every
# target that needs the tinyusb target (splc_components, once
# usb_cdc/sx_usb_cdc.c is wired in components/CMakeLists.txt) will fail
# to configure until the submodule is present.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tinyusb/src/tusb.c)
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
    #
    # NOTE the path here is ../port/usb, not ../TUSB -- usb_descriptors.c
    # and tusb_config.h live in this repo's own port/usb/ directory (see
    # port/usb/tusb_config.h's header comment), not in a top-level TUSB/
    # folder (no such folder exists in this repo).
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
    # USB peripheral init header dcd_stm32_fsdev.c and
    # components/usb_cdc/sx_usb_cdc.c both need). PUBLIC so anything
    # linking "tinyusb" transitively resolves these too.
    target_link_libraries(tinyusb PUBLIC
        stm32cubemx
    )

    target_compile_features(tinyusb PUBLIC c_std_11)

    message(STATUS "libs/CMakeLists.txt: tinyusb submodule found -- 'tinyusb' target built (stm32_fsdev driver, this project's port/usb/ descriptors).")
else()
    message(STATUS "libs/CMakeLists.txt: tinyusb submodule not checked out (libs/tinyusb is empty) -- run 'git submodule update --init --recursive'. No 'tinyusb' target created; anything needing tusb.h will fail to configure until this is done.")
endif()