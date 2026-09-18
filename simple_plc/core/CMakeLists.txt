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
    plc_internal_rule/plc_rule_eval.c
    plc_internal_rule/plc_rule_action.c
)

# plc_rule.c now calls log_debug()/log_info()/log_warn() (utils/logger/
# logger.h) to make rule-commit and rule-fire events observable -- see
# that file's own comment. This only needs logger.h's declarations to
# compile; it deliberately does NOT add logger.c as a source here.
# logger.c is already compiled exactly once, into splc_board (see
# board/CMakeLists.txt's own comment on why it lives there), and every
# target this project produces links splc_core and splc_board into the
# same final executable (see this project's root CMakeLists.txt) -- so
# splc_board's logger.c already supplies these symbols at final link
# time. Compiling logger.c a second time into splc_core as well would
# give the linker two definitions of log_debug()/logger_init()/etc. and
# fail with a duplicate-symbol error.
#
# This does NOT reintroduce a Layer 0/1 dependency into Layer 2: logger.h
# itself is pure Layer U (stdint.h/stdbool.h only, callback-based output
# via p_log_func -- see its own header comment), so splc_core still
# builds and links standalone on a plain PC as long as *some* logger.c
# (or a test stub providing the same symbols) is linked in alongside it
# -- exactly the same situation plc_tag.c/plc_rule.c already have with
# any other Layer U utility they might someday call.
target_include_directories(splc_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../utils/logger
)

# plc_device/, plc_error/, plc_system_cmd/ are each header-only (struct/enum
# type definitions, no matching .c -- see each header's own comment for why).
# These must stay PUBLIC, not PRIVATE: their entire purpose is to be
# #include'd by Layer 3/4 code (services/, app/) that links against
# splc_core. PRIVATE would hide them from splc_core's exported include
# interface and break every Layer 3/4 caller.
target_include_directories(splc_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/plc_tag
    ${CMAKE_CURRENT_SOURCE_DIR}/plc_rule
    ${CMAKE_CURRENT_SOURCE_DIR}/plc_device
    ${CMAKE_CURRENT_SOURCE_DIR}/plc_error
    ${CMAKE_CURRENT_SOURCE_DIR}/plc_system_cmd
)

# plc_rule_eval.h / plc_rule_action.h / plc_rule_state_machine.h are internal
# to plc_rule.c (see their own header comments) and are not meant to be
# included by callers outside core/. PRIVATE keeps this path out of
# splc_core's exported include interface, while still letting plc_rule.c's
# own #include "plc_rule_eval.h" etc. resolve during splc_core's own build.
target_include_directories(splc_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/plc_internal_rule
)

target_compile_features(splc_core PUBLIC c_std_11)