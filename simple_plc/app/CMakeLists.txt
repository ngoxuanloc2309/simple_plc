# app/CMakeLists.txt - Layer 4 (Engine & Application entry)
#
# Builds plc_engine.c (app/plc_app/) as a static library target:
# splc_app. Same layering approach as every other layer's CMakeLists.txt
# (core/, components/, platforms/stm32/stm32h5/, port/, services/,
# board/) -- one small target per layer, so a future plain-PC unit test
# binary can link only the layers it needs without pulling in the STM32
# HAL or this file's own board/USB dependencies.
#
# app_config.h and sx_platform_config.h (app/, not app/plc_app/) are
# NOT Layer 4 -- they are project-wide compile-time configuration
# (STM32H5_PLATFORM etc.) included from Layer 0/1/3 headers throughout
# the tree (e.g. every platforms/stm32/stm32h5/*.h). Their include dir
# is exposed here as PUBLIC only because splc_app is the target most
# other things link against at the top; it is not a statement that
# app_config.h/sx_platform_config.h conceptually belong to Layer 4.

add_library(splc_app STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/plc_app/plc_engine.c
    ${CMAKE_CURRENT_SOURCE_DIR}/plc_app/plc_system_cmd_service.c
)

target_include_directories(splc_app PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/plc_app
)

# splc_core (Layer 2) supplies plc_tag.h/plc_rule.h, which plc_engine.c
# calls directly (tag_table_load_from_flash(), rule_table_load_from_flash(),
# rule_scan()).
target_link_libraries(splc_app PUBLIC
    splc_core
)

# splc_services (Layer 3) supplies plc_io.h/plc_retain.h/plc_modbus_cfg.h,
# which plc_engine.c calls directly (input_scan()/output_scan(),
# retain_store_restore()/retain_service(), plc_modbus_cfg_init()/
# modbus_config_service()/plc_modbus_cfg_record_scan_time()).
target_link_libraries(splc_app PUBLIC
    splc_services
)

# splc_board (Layer 4, board/) supplies board.h (board_init(),
# board_get_modbus_transport()), which plc_engine.c calls directly.
# Transitively brings in splc_components (sx_time.h's sx_get_tick_ms(),
# also called directly by plc_engine.c, AND sx_system.h's
# sx_system_reset(), called directly by plc_system_cmd_service.c) and
# splc_port (modbus_transport.h) via splc_board's own PUBLIC links -- not
# repeated here.
target_link_libraries(splc_app PUBLIC
    splc_board
)

target_compile_features(splc_app PUBLIC c_std_11)