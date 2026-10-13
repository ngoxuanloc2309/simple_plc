# utils/CMakeLists.txt - Layer U (pure algorithms, no hardware dependency)
#
# splc_utils: SimplePLC's own queue, epoch and MQTT payload formatting helpers. Linked into
# splc_components, so every layer above gets the headers and the symbols
# without the top-level executable having to compile any of these files.
# splc_cqueue_* carries the prefix on purpose: a host firmware usually has its
# own cqueue (same API, same file name) and the two must not collide.

add_library(splc_utils STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/cqueue/splc_cqueue.c
    ${CMAKE_CURRENT_SOURCE_DIR}/epoch/splc_epoch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mqtt_fmt/splc_mqtt_fmt.c
)
target_include_directories(splc_utils PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/cqueue
    ${CMAKE_CURRENT_SOURCE_DIR}/epoch
    ${CMAKE_CURRENT_SOURCE_DIR}/mqtt_fmt
)
target_compile_features(splc_utils PUBLIC c_std_11)