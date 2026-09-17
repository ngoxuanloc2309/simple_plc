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
# UNLIKE nanoMODBUS above, libs/tinyusb is a git submodule
# (see /.gitmodules: path simple_plc/libs/tinyusb, url
# https://github.com/hathach/tinyusb.git) that is NOT checked out in this
# working copy -- the directory exists but is completely empty (0 files).
# `git submodule update --init --recursive` must be run from the repo
# root before there is anything here to build at all.
#
# The guard below means: if the submodule hasn't been fetched yet,
# `cmake configure` still succeeds (skips TinyUSB) instead of failing
# with a confusing "source file does not exist" error -- but every
# target that would have needed the tinyusb target silently doesn't get
# it either. components/usb_cdc/sx_usb_cdc.c (Layer 1) `#include
# "tusb.h"` under `#if STM32H5_PLATFORM` -- so once wired, whatever
# target compiles that file (currently NOT wired into
# components/CMakeLists.txt either, see the TODO there) will need to
# link this "tinyusb" target once it exists.
#
# Deliberately NOT attempting to build TinyUSB's sources here yet, even
# behind the EXISTS guard: TinyUSB needs its own genuinely nontrivial
# CMake wiring (its own tusb.c/usbd.c/cdc_device.c common sources, plus
# exactly one dcd_*.c device-controller driver matching this MCU --
# dcd_synopsys.c for STM32H5's USB peripheral -- selected via TinyUSB's
# own family/board config headers, distinct from and in addition to this
# project's own port/usb/tusb_config.h). Getting that selection wrong
# silently would be worse than leaving it as an explicit next step here.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tinyusb/src/tusb.c)
    message(STATUS "libs/CMakeLists.txt: tinyusb submodule found, but not yet wired up (see comment in libs/CMakeLists.txt) -- no 'tinyusb' target created.")
else()
    message(STATUS "libs/CMakeLists.txt: tinyusb submodule not checked out (libs/tinyusb is empty) -- run 'git submodule update --init --recursive'. No 'tinyusb' target created; anything needing tusb.h will fail to build until this is done.")
endif()