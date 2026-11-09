# DTU_Zigbee/CMakeLists.txt - product port of the DTU Zigbee (STM32H563, SX-ZB-ETH-V2.0),
# outside simple_plc/ (same idea as Zigbee_IO/CMakeLists.txt). Host firmware: eth_log_board.
# (Was stored as DTU_Zigbee/port/CMakeLists.txt but its paths assume DTU_Zigbee/CMakeLists.txt.)
#
# splc_dtu_port: glue between the host firmware (lwIP, RTC, MQTT client, FreeRTOS, sx_uart) and the
# library's registration points. The USB-CDC App link is NOT here: the library's own module
# port/usb (SPLC_PORT_USB_CDC = 1) owns USB with its TinyUSB. Logger and UARTs belong to the host.
# It also compiles ONE port that already exists in Zigbee_IO/port/, unchanged (empty here while
# SPLC_INDICATOR_ENABLE = 0):
#   - splc_indicators_port.c
# Zigbee_IO's splc_zigbee_port.c is NOT used: it brings its UART up through sx_uart.
#
# OBJECT library on purpose (handoff lesson 87): the board file calls into these objects and sits
# EARLIER on the link line; a STATIC library failed to link for that reason. The host adds
# $<TARGET_OBJECTS:splc_dtu_port> to its executable (see the integration lines at the end).
#
# Added by the host's CMakeLists AFTER add_subdirectory(<simple_plc>).

set(SPLC_ZIGBEE_IO_PORT_DIR "${CMAKE_CURRENT_LIST_DIR}/../Zigbee_IO/port" CACHE PATH
    "Directory of splc_indicators_port.c + splc_zigbee_port.h (Zigbee_IO/port)")
foreach(f splc_zigbee_port.h splc_indicators_port.c)
    if(NOT EXISTS "${SPLC_ZIGBEE_IO_PORT_DIR}/${f}")
        message(FATAL_ERROR "DTU_Zigbee: ${SPLC_ZIGBEE_IO_PORT_DIR}/${f} not found "
                            "(set -DSPLC_ZIGBEE_IO_PORT_DIR=<dir>)")
    endif()
endforeach()

add_library(splc_dtu_port OBJECT
    ${CMAKE_CURRENT_LIST_DIR}/port/splc_port.c
    ${CMAKE_CURRENT_LIST_DIR}/port/splc_tcp_port.c
    ${CMAKE_CURRENT_LIST_DIR}/port/splc_rtc_port.c
    ${CMAKE_CURRENT_LIST_DIR}/port/splc_mqtt_port.c
    ${CMAKE_CURRENT_LIST_DIR}/port/splc_host_rs485_port.c
    ${CMAKE_CURRENT_LIST_DIR}/port/splc_host_zigbee_port.c
    ${SPLC_ZIGBEE_IO_PORT_DIR}/splc_indicators_port.c
)

target_include_directories(splc_dtu_port PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/port
    ${SPLC_ZIGBEE_IO_PORT_DIR}
)

# splc_config: splcopts.h + splc_opt.h / splc_log.h.  splc_services: plc_*.h.  splc_port:
# modbus_link.h / modbus_transport.h.  splc_components: sx_time.h.  splc_app: plc_engine.h.
# stm32cubemx: Core/Inc (rtc.h, main.h, usart.h), HAL, FreeRTOS headers.
# NOTE: the host's board/dev/uart/sx_uart.h (bsp target) has the same file name as the library's
# components/uart/sx_uart.h; the RS485 port includes it as "dev/uart/sx_uart.h". The bsp target
# must be linked so that its include directory (board/) is visible.
target_link_libraries(splc_dtu_port PUBLIC
    splc_config
    splc_services
    splc_port
    splc_components
    splc_app
    stm32cubemx
)
foreach(t bsp lwip)
    if(TARGET ${t})
        target_link_libraries(splc_dtu_port PUBLIC ${t})
    endif()
endforeach()

target_compile_features(splc_dtu_port PUBLIC c_std_11)

# ---- Integration in the host's root CMakeLists.txt (eth_log_board), in this order ----
#   set(SPLC_AS_LIBRARY ON CACHE BOOL "" FORCE)
#   set(SPLC_OPTS_DIR "${CMAKE_SOURCE_DIR}/DTU_Zigbee/port" CACHE PATH "" FORCE)
#   add_subdirectory(<where simple_plc/ is>)         # builds the INTERFACE target 'splc'
#   add_subdirectory(DTU_Zigbee)                     # this file
#   target_sources(${CMAKE_PROJECT_NAME} PRIVATE $<TARGET_OBJECTS:splc_dtu_port>)
#   target_link_libraries(${CMAKE_PROJECT_NAME} ... splc)   # same (plain) signature as the host's