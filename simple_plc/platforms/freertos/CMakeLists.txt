# platforms/freertos/CMakeLists.txt - Layer 0 (Platform)
#
# FreeRTOS implementation of the Layer 1 OS contract (components/os/sx_os.h).
# Not chip specific, so it sits next to platforms/stm32/ rather than in it.
#
# The source compiles to nothing unless the product option
# SX_OS_USE_FREERTOS (splcopts.h) is 1, so the library always builds this
# target and the option alone decides whether an RTOS is used.
#
# OBJECT library on purpose: simple_plc/CMakeLists.txt adds its object file
# straight into the executable ($<TARGET_OBJECTS:...>). A static library
# would depend on link order (earlier libraries call sx_os_*()).
#
# RTOS headers (FreeRTOS.h, task.h, FreeRTOSConfig.h) come from the HOST
# project and are only needed when SX_OS_USE_FREERTOS = 1:
#   cmake -DSPLC_OS_INCLUDE_DIRS="path/to/FreeRTOS/include;path/to/port;path/to/config"
set(SPLC_OS_INCLUDE_DIRS "" CACHE STRING "Include dirs of the host RTOS (FreeRTOS.h, task.h, FreeRTOSConfig.h)")

add_library(splc_platform_freertos OBJECT
    ${CMAKE_CURRENT_SOURCE_DIR}/sx_os_freertos.c
)

# sx_os.h (components/os/) and, through components' PUBLIC include dirs,
# the app/ and config/ headers.
target_link_libraries(splc_platform_freertos PRIVATE splc_components)

if(SPLC_OS_INCLUDE_DIRS)
    target_include_directories(splc_platform_freertos PRIVATE ${SPLC_OS_INCLUDE_DIRS})
endif()