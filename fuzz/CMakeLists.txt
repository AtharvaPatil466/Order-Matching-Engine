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
        target_compile_options(${name} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer -g)
        target_link_options(${name} PRIVATE
            -fsanitize=address,undefined)
    endif()
endfunction()

add_replay_target(fuzz_fix_parser_replay     fuzz_fix_parser.cpp)
add_replay_target(fuzz_fix_roundtrip_replay  fuzz_fix_roundtrip.cpp)
add_replay_target(fuzz_fix_framer_replay     fuzz_fix_framer.cpp)

# ---- libFuzzer binary (optional) ------------------------------------------

set(_FUZZER_FLAG "-fsanitize=fuzzer,address,undefined")
set(CMAKE_REQUIRED_FLAGS "${_FUZZER_FLAG}")
check_cxx_compiler_flag("${_FUZZER_FLAG}" HAVE_LIBFUZZER)
unset(CMAKE_REQUIRED_FLAGS)

function(add_libfuzzer_target name harness_src)
    add_executable(${name} ${harness_src})
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/include)
    target_compile_features(${name} PRIVATE cxx_std_20)
    target_compile_options(${name} PRIVATE
        ${_FUZZER_FLAG} -fno-omit-frame-pointer -g)
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
