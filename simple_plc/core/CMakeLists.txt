# core/CMakeLists.txt - Layer 2 (PLC Core)
#
# Builds the Tag Table + Rule Engine as an independent static library,
# splc_core. This is the designated build/port boundary (see
# docs/architecture.md section 1, rule 3): it must not link against, or
# include headers from, any Layer 0/1 target (splc_platform, driver
# components) or any third-party library. This is what lets splc_core be
# built and unit tested on a plain PC toolchain, with no target hardware.
#
# Anyone adding target_link_libraries(splc_core PRIVATE <layer0/1 target>)
# here is violating the layering rule -- that should be caught in review.

add_library(splc_core STATIC
    plc_tag/plc_tag.c
    plc_rule/plc_rule.c
    plc_internal/plc_rule_eval.c
    plc_internal/plc_rule_action.c
)

target_include_directories(splc_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/plc_tag
    ${CMAKE_CURRENT_SOURCE_DIR}/plc_rule
    ${CMAKE_CURRENT_SOURCE_DIR}/plc_device
)

# plc_rule_eval.h / plc_rule_action.h / plc_rule_state_machine.h are internal
# to plc_rule.c (see their own header comments) and are not meant to be
# included by callers outside core/. PRIVATE keeps this path out of
# splc_core's exported include interface, while still letting plc_rule.c's
# own #include "plc_rule_eval.h" etc. resolve during splc_core's own build.
target_include_directories(splc_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/plc_internal
)

target_compile_features(splc_core PUBLIC c_std_11)