# Fuzzing harness for the FIX parser and engine input boundaries.
#
# Two targets are defined:
#   fuzz_fix_parser_replay   Always built. Standalone driver that runs the
#                            harness against files passed on the command line.
#                            Use this in CI / on toolchains without libFuzzer.
#   fuzz_fix_parser          Built only if the compiler supports
#                            -fsanitize=fuzzer (mainline Clang). Real coverage-
#                            guided fuzzer.

include(CheckCXXCompilerFlag)

# ---- Replay binary ---------------------------------------------------------

function(add_replay_target name harness_src)
    add_executable(${name} ${harness_src} standalone_main.cpp)
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/include)
    target_compile_features(${name} PRIVATE cxx_std_20)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        # -UNDEBUG: the harnesses' asserts are their oracle. A Release
        # configure compiled them out — and with them the parse calls written
        # inside assert() — which GCC's -Werror then refused as unused.
        target_compile_options(${name} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer -g -UNDEBUG)
        target_link_options(${name} PRIVATE
            -fsanitize=address,undefined)
    endif()
endfunction()

add_replay_target(fuzz_fix_parser_replay     fuzz_fix_parser.cpp)
add_replay_target(fuzz_fix_roundtrip_replay  fuzz_fix_roundtrip.cpp)
add_replay_target(fuzz_fix_framer_replay     fuzz_fix_framer.cpp)

# ---- libFuzzer binary (optional) ------------------------------------------

set(_FUZZER_FLAG "-fsanitize=fuzzer,address,undefined")
# Probe with an entry point. check_cxx_compiler_flag links its own int main(),
# which clashes with libFuzzer's: GNU ld refuses the two mains, so on Linux the
# probe failed with a working libFuzzer and no libFuzzer target ever built
# there (macOS ld64 let it through).
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "${_FUZZER_FLAG}")
check_cxx_source_compiles("
#include <cstddef>
#include <cstdint>
extern \"C\" int LLVMFuzzerTestOneInput(const uint8_t*, size_t) { return 0; }
" HAVE_LIBFUZZER)
unset(CMAKE_REQUIRED_FLAGS)

function(add_libfuzzer_target name harness_src)
    add_executable(${name} ${harness_src})
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/include)
    target_compile_features(${name} PRIVATE cxx_std_20)
    target_compile_options(${name} PRIVATE
        ${_FUZZER_FLAG} -fno-omit-frame-pointer -g -UNDEBUG)
    target_link_options(${name} PRIVATE ${_FUZZER_FLAG})
endfunction()

if(HAVE_LIBFUZZER)
    add_libfuzzer_target(fuzz_fix_parser    fuzz_fix_parser.cpp)
    add_libfuzzer_target(fuzz_fix_roundtrip fuzz_fix_roundtrip.cpp)
    add_libfuzzer_target(fuzz_fix_framer    fuzz_fix_framer.cpp)
    message(STATUS "libFuzzer available — building libFuzzer targets")
else()
    message(STATUS "libFuzzer not available with this toolchain — only replay binaries will be built")
endif()

# ---- Engine target: wire bytes -> TcpGateway -> submitOrder -> integrity ----
#
# fuzz_gateway_engine drives the engine itself, so it is compiled TOGETHER with
# the OrderMatcher sources under its own flags: libFuzzer only steers by
# coverage it instrumented, and the sanitizers only see code they compiled.
# -UNDEBUG keeps the engine's asserts as an oracle; UBSan halts.
get_target_property(_om_sources OrderMatcher SOURCES)
list(TRANSFORM _om_sources PREPEND "${CMAKE_SOURCE_DIR}/")

function(add_engine_fuzz_target name flags)
    add_executable(${name} fuzz_gateway_engine.cpp ${_om_sources} ${ARGN})
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/include)
    target_compile_features(${name} PRIVATE cxx_std_20)
    target_compile_options(${name} PRIVATE ${flags} -fno-sanitize-recover=all
                           -fno-omit-frame-pointer -g -UNDEBUG)
    target_link_options(${name} PRIVATE ${flags})
    target_link_libraries(${name} PRIVATE Threads::Threads)
endfunction()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_engine_fuzz_target(fuzz_gateway_engine_replay "-fsanitize=address,undefined"
                           standalone_main.cpp)
endif()
if(HAVE_LIBFUZZER)
    add_engine_fuzz_target(fuzz_gateway_engine "${_FUZZER_FLAG}")
endif()
