# simple_plc/cmake_modules/splc_select.cmake
#
# Chooses the board source and the Layer 0 platform from the PRODUCT's
# splcopts.h, so splcopts.h stays the single source of truth (no -DSPLC_BOARD_SKU,
# no hard-coded add_subdirectory(platforms/...)).
#
# Reads   SPLC_BOARD / SPLC_PLATFORM  the same way the C preprocessor would:
#           1. an uncommented "#define SPLC_BOARD SPLC_BOARD_xxx" in splcopts.h
#           2. otherwise the library default in config/splc_opt.h
# Sets    SPLC_BOARD_SKU          -> board/board_device/board_<sku>.c
#         SPLC_PLATFORM_DIR       -> platforms/<dir> (holds a CMakeLists.txt)
#         SPLC_PLATFORM_TARGET    -> Layer 0 library target name
#         SPLC_LOGGER_*           -> built-in logger source / include dir
#         SPLC_PORT_USB_CDC       -> 1 when the optional USB-CDC port module is built
#
# Adding a board : add one row to SPLC_BOARD_TABLE (and the selector in
#                  config/splc_opt.h + board_config.h), then write board_<sku>.c/.h.
# Adding a chip  : add one row to SPLC_PLATFORM_TABLE, then write
#                  platforms/<dir>/CMakeLists.txt that defines <target>.

# Row format: "<selector macro name>|<value>"
set(SPLC_BOARD_TABLE
    "SPLC_BOARD_ZIGBEE_IO_4DI_4DO|zigbee_io"
    "SPLC_BOARD_REMOTE_IO_8DI_8DO|remote_io_8di_8do"
    "SPLC_BOARD_DATALOGGER|datalogger"
    "SPLC_BOARD_GATEWAY|ethernet_wifi_gateway"
)
# Row format: "<selector macro name>|<dir under platforms/>|<library target>"
set(SPLC_PLATFORM_TABLE
    "SPLC_PLATFORM_STM32H5|stm32/stm32h5|splc_platform_stm32h5"
    "SPLC_PLATFORM_STM32F1|stm32/stm32f1|splc_platform_stm32f1"
    "SPLC_PLATFORM_STM32F4|stm32/stm32f4|splc_platform_stm32f4"
    "SPLC_PLATFORM_STM32H7|stm32/stm32h7|splc_platform_stm32h7"
    "SPLC_PLATFORM_ESP32|esp32|splc_platform_esp32"
)

# splc_read_option(<option name> <prefix> <out var>)
# Finds "#define <option> <prefix>xxx" at the start of a line (so a commented
# line is ignored, like the preprocessor). splcopts.h wins over splc_opt.h.
function(splc_read_option option prefix out_var)
    foreach(f "${SPLC_OPTS_DIR}/splcopts.h" "${SPLC_LIB_DIR}/config/splc_opt.h")
        file(STRINGS "${f}" lines
             REGEX "^[ \t]*#[ \t]*define[ \t]+${option}[ \t]+${prefix}[A-Za-z0-9_]*")
        if(lines)
            list(GET lines 0 first)
            string(REGEX REPLACE "^.*[ \t](${prefix}[A-Za-z0-9_]*).*$" "\\1" value "${first}")
            set(${out_var} "${value}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    message(FATAL_ERROR
        "SimplePLC: cannot find '#define ${option} ${prefix}...' in "
        "${SPLC_OPTS_DIR}/splcopts.h or ${SPLC_LIB_DIR}/config/splc_opt.h")
endfunction()

# splc_read_flag(<option name> <out var>)
# Same lookup for a numeric 0/1 option ("#define <option> 1"); result is 0 or 1.
function(splc_read_flag option out_var)
    foreach(f "${SPLC_OPTS_DIR}/splcopts.h" "${SPLC_LIB_DIR}/config/splc_opt.h")
        file(STRINGS "${f}" lines
             REGEX "^[ \t]*#[ \t]*define[ \t]+${option}[ \t]+[01]([^A-Za-z0-9_]|$)")
        if(lines)
            list(GET lines 0 first)
            string(REGEX REPLACE "^.*[ \t]([01])([^A-Za-z0-9_].*)?$" "\\1" value "${first}")
            set(${out_var} "${value}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    message(FATAL_ERROR "SimplePLC: cannot find '#define ${option} 0|1' in splcopts.h or splc_opt.h")
endfunction()

# splc_lookup(<table var> <selector> <out var> <field count>)
function(splc_lookup table selector out_var what)
    foreach(row IN LISTS ${table})
        string(REPLACE "|" ";" cols "${row}")
        list(GET cols 0 key)
        if(key STREQUAL selector)
            set(${out_var} "${cols}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(known "")
    foreach(row IN LISTS ${table})
        string(REPLACE "|" ";" cols "${row}")
        list(GET cols 0 key)
        string(APPEND known "\n  ${key}")
    endforeach()
    message(FATAL_ERROR
        "SimplePLC: ${what} '${selector}' (splcopts.h) is not a known selector.\n"
        "Known selectors:${known}")
endfunction()

set(SPLC_LIB_DIR "${CMAKE_CURRENT_LIST_DIR}/..")

# --- Board ---------------------------------------------------------------
splc_read_option(SPLC_BOARD "SPLC_BOARD_" _splc_board_sel)
splc_lookup(SPLC_BOARD_TABLE "${_splc_board_sel}" _splc_board_row "SPLC_BOARD")
list(GET _splc_board_row 1 SPLC_BOARD_SKU)
set(_splc_board_src "${SPLC_LIB_DIR}/board/board_device/board_${SPLC_BOARD_SKU}.c")
if(NOT EXISTS "${_splc_board_src}")
    message(FATAL_ERROR
        "SimplePLC: SPLC_BOARD=${_splc_board_sel} selects board '${SPLC_BOARD_SKU}' but\n"
        "  ${_splc_board_src}\n"
        "does not exist yet (board not implemented).")
endif()

# --- Platform (Layer 0) --------------------------------------------------
splc_read_option(SPLC_PLATFORM "SPLC_PLATFORM_" _splc_plat_sel)
splc_lookup(SPLC_PLATFORM_TABLE "${_splc_plat_sel}" _splc_plat_row "SPLC_PLATFORM")
list(GET _splc_plat_row 1 SPLC_PLATFORM_DIR)
list(GET _splc_plat_row 2 SPLC_PLATFORM_TARGET)
if(NOT EXISTS "${SPLC_LIB_DIR}/platforms/${SPLC_PLATFORM_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "SimplePLC: SPLC_PLATFORM=${_splc_plat_sel} selects platforms/${SPLC_PLATFORM_DIR}, "
        "which has no CMakeLists.txt yet (platform not implemented).")
endif()

message(STATUS "SimplePLC board    : ${_splc_board_sel} -> board_${SPLC_BOARD_SKU}.c")
message(STATUS "SimplePLC platform : ${_splc_plat_sel} -> platforms/${SPLC_PLATFORM_DIR} (${SPLC_PLATFORM_TARGET})")
# --- Logger --------------------------------------------------------------
# SPLC_LOGGER_BUILTIN 1 (default): build utils/logger/logger.c and expose its
# logger.h. 0: the host firmware supplies logger.h and the logger (see
# config/splc_log.h) -- SimplePLC then compiles no logger.c and must not put
# its own logger.h on any include path (same file name as the host's).
splc_read_flag(SPLC_LOGGER_BUILTIN SPLC_LOGGER_BUILTIN)
if(SPLC_LOGGER_BUILTIN)
    set(SPLC_LOGGER_SRC          "${SPLC_LIB_DIR}/utils/logger/logger.c")
    set(SPLC_LOGGER_INCLUDE_DIR  "${SPLC_LIB_DIR}/utils/logger")
else()
    set(SPLC_LOGGER_SRC          "")
    set(SPLC_LOGGER_INCLUDE_DIR  "")
endif()
message(STATUS "SimplePLC logger   : ${SPLC_LOGGER_BUILTIN} (1 = built-in utils/logger, 0 = host firmware's)")

# --- RTC / ADC drivers --------------------------------------------------
# SPLC_RTC_BUILTIN / SPLC_ADC_BUILTIN 1 (default): build the STM32H5 driver
# (needs the CubeMX rtc.h / adc.h of the project). 0: the host firmware
# supplies the sx_rtc_* / sx_adc_* functions itself (config/splc_opt.h).
splc_read_flag(SPLC_RTC_BUILTIN SPLC_RTC_BUILTIN)
splc_read_flag(SPLC_ADC_BUILTIN SPLC_ADC_BUILTIN)
message(STATUS "SimplePLC RTC      : ${SPLC_RTC_BUILTIN} (1 = built-in STM32H5 RTC driver, 0 = host's sx_rtc_*)")
message(STATUS "SimplePLC ADC      : ${SPLC_ADC_BUILTIN} (1 = built-in STM32H5 ADC driver, 0 = host's sx_adc_*)")

# --- USB-CDC port module ------------------------------------------------
# SPLC_PORT_USB_CDC 1: build the optional USB-CDC module (port/usb/ + TinyUSB
# in libs/). 0 (default): no USB code at all -- the host firmware owns the USB
# peripheral (see config/splc_opt.h and docs/architecture.md section 3.1).
splc_read_flag(SPLC_PORT_USB_CDC SPLC_PORT_USB_CDC)
message(STATUS "SimplePLC USB-CDC  : ${SPLC_PORT_USB_CDC} (1 = SimplePLC owns USB: TinyUSB + port/usb, 0 = host's)")