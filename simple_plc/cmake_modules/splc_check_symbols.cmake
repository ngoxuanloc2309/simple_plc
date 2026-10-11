# splc_check_symbols.cmake - run as a POST_BUILD step (cmake -P):
#
#   cmake -DNM=<nm tool> -DLIB=<static library> -P splc_check_symbols.cmake
#
# Fails the build if the library defines any external symbol that does NOT
# start with "splc_". It guards SimplePLC's private nanoMODBUS copy
# (libs/splc_nanomodbus/splc_nmbs.h): that copy only avoids "multiple
# definition" against a host's own nanoMODBUS as long as EVERY global symbol
# is renamed. A new nanoMODBUS version that adds a global function, or a typo
# in the rename list, would otherwise show up only later, at the host's link,
# as a confusing duplicate-symbol error.

if(NOT NM OR NOT LIB)
    message(FATAL_ERROR "splc_check_symbols: pass -DNM=<nm> -DLIB=<library>")
endif()

execute_process(
    COMMAND ${NM} --defined-only --extern-only ${LIB}
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE  _err
    RESULT_VARIABLE _rc
)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "splc_check_symbols: '${NM}' failed on ${LIB}: ${_err}")
endif()

string(REPLACE "\n" ";" _lines "${_out}")
set(_bad "")
set(_count 0)
foreach(_line IN LISTS _lines)
    # "<address> <type> <name>"; the per-object "file.o:" headers and blank
    # lines of an archive listing do not match and are skipped.
    if(_line MATCHES "^[0-9a-fA-F]+ ([A-Za-z]) (.+)$")
        math(EXPR _count "${_count} + 1")
        set(_name "${CMAKE_MATCH_2}")
        if(NOT _name MATCHES "^splc_")
            list(APPEND _bad "${_name}")
        endif()
    endif()
endforeach()

if(_count EQUAL 0)
    message(FATAL_ERROR "splc_check_symbols: no symbols found in ${LIB} (wrong file?)")
endif()
if(_bad)
    string(REPLACE ";" "\n    " _bad_text "${_bad}")
    message(FATAL_ERROR
        "splc_check_symbols: ${LIB} exports symbols without the splc_ prefix:\n"
        "    ${_bad_text}\n"
        "Add a '#define <name> splc_<name>' for each to libs/splc_nanomodbus/splc_nmbs.h.")
endif()
message(STATUS "splc_check_symbols: ${_count} external symbols, all splc_-prefixed")