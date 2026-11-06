# Zigbee_IO/CMakeLists.txt - product port of the Zigbee-IO board (outside simple_plc/)
#
# splc_zigbee_port: the glue between the DRF1609H module (USART2) and the library's
# plc_zigbee block. Empty unless SPLC_ZIGBEE_ENABLE = 1 in splcopts.h.
#
# OBJECT library on purpose, same pattern as splc_platform_freertos: the root
# CMakeLists.txt adds $<TARGET_OBJECTS:splc_zigbee_port> to the executable, so the
# symbol splc_zigbee_port_init() (called from splc_board, which sits earlier on the
# link line) resolves whatever the static-library order is. A STATIC library here
# failed to link: libsplc_board.a is pulled in again AFTER it.
#
# Added by the root CMakeLists.txt AFTER add_subdirectory(simple_plc): it links the
# simple_plc targets for their include paths.
add_library(splc_zigbee_port OBJECT
    ${CMAKE_CURRENT_SOURCE_DIR}/port/splc_zigbee_port.c
)

target_include_directories(splc_zigbee_port PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/port
)

# splc_config: splcopts.h (SPLC_ZIGBEE_ENABLE) + splc_opt.h / splc_log.h.
# splc_services: plc_zigbee.h.  splc_port: modbus_serial.h.  splc_components: sx_uart.h.
# stm32cubemx: Core/Inc/usart.h (huart2) and the HAL headers.
target_link_libraries(splc_zigbee_port PUBLIC
    splc_config
    splc_services
    splc_port
    splc_components
    stm32cubemx
)

target_compile_features(splc_zigbee_port PUBLIC c_std_11)


# splc_indicators_port: the three LEDs (POWER / STATUS / LINK) of the connection indicator.
# Empty unless SPLC_INDICATOR_ENABLE = 1. OBJECT library for the same reason as above: the
# symbol splc_indicators_port_init() is called from splc_board (earlier on the link line).
add_library(splc_indicators_port OBJECT
    ${CMAKE_CURRENT_SOURCE_DIR}/port/splc_indicators_port.c
)
target_include_directories(splc_indicators_port PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/port
)
target_link_libraries(splc_indicators_port PUBLIC
    splc_config splc_services splc_port splc_components stm32cubemx
)
target_compile_features(splc_indicators_port PUBLIC c_std_11)