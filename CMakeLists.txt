cmake_minimum_required(VERSION 3.15)
project(OrderMatchingEngine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(ENABLE_THREAD_SANITIZER "Enable ThreadSanitizer" OFF)
option(ENABLE_LTO "Enable link-time optimization in Release builds" OFF)
option(ENABLE_ASSERTIONS "Keep C assert checks enabled in all build types" ON)
option(ENABLE_FAULT_INJECTION "Compile fault-injection points (zero overhead when off)" OFF)
# Clang IR-based profile-guided optimization (opt-in; driven by scripts/pgo_build.sh).
option(ENABLE_PGO "Enable Clang IR-based profile-guided optimization" OFF)
set(PGO_MODE ""    CACHE STRING "PGO phase when ENABLE_PGO=ON: 'generate' or 'use'")
set(PGO_PROFILE "" CACHE STRING "Absolute path to merged .profdata for PGO_MODE=use")
if(ENABLE_SANITIZERS AND ENABLE_THREAD_SANITIZER)
    message(FATAL_ERROR "ENABLE_SANITIZERS and ENABLE_THREAD_SANITIZER cannot be enabled together")
endif()
if(ENABLE_SANITIZERS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    else()
        message(WARNING "ENABLE_SANITIZERS is only configured for Clang/GNU")
    endif()
endif()
if(ENABLE_THREAD_SANITIZER)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
        add_link_options(-fsanitize=thread)
    else()
        message(WARNING "ENABLE_THREAD_SANITIZER is only configured for Clang/GNU")
    endif()
endif()

# Aggressive Optimizations for HFT
if(CMAKE_BUILD_TYPE MATCHES "Release")
    message(STATUS "Enabling HFT Optimizations: -O3 -march=native -mtune=native")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -march=native -mtune=native -Wall -Wextra -Werror")
    # Warnings that depend on the HOST (compiler version or CPU) rather than on
    # our code must not be fatal: -march=native means the same source produces
    # different diagnostics on different machines, so -Werror turns any of these
    # into a build that fails only on some runners.
    #   character-conversion         only exists in newer Clang (Apple clang has
    #                                it; clang-18 in the chaos image does not,
    #                                and rejects the unknown flag as an error
    #                                under -Werror — this silently killed the
    #                                chaos image build for weeks).
    #   invalid-feature-combination  -march=native on an AVX10.1-capable runner
    #                                expands to +avx10.1-256, which Clang warns
    #                                it will promote to -512. GitHub's runner
    #                                pool is heterogeneous, so this failed the
    #                                chaos build only on the days it happened to
    #                                land on newer Intel silicon.
    # Each is demoted only where the compiler knows the warning; where it
    # doesn't, the warning can't fire so no demotion is needed.
    include(CheckCXXCompilerFlag)
    foreach(_host_warn character-conversion invalid-feature-combination)
        string(MAKE_C_IDENTIFIER "HAS_WNO_ERROR_${_host_warn}" _host_warn_var)
        check_cxx_compiler_flag("-Wno-error=${_host_warn}" ${_host_warn_var})
        if(${_host_warn_var})
            set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -Wno-error=${_host_warn}")
        endif()
    endforeach()
    
    if(ENABLE_LTO)
        # Link Time Optimization (LTO)
        include(CheckIPOSupported)
        check_ipo_supported(RESULT result OUTPUT output)
        if(result)
            set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
            message(STATUS "LTO Enabled")
        else()
            message(WARNING "LTO not supported: ${output}")
        endif()
    endif()
endif()

# ─── Profile-Guided Optimization (Clang IR-based PGO) ───────────────────────
# Opt-in, driven by scripts/pgo_build.sh. Must be set BEFORE any target is
# defined so the compile/link flags apply to OrderMatcher and the benchmarks.
# When ENABLE_PGO=OFF (default) this block is skipped entirely — the normal
# build is unchanged.
if(ENABLE_PGO)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "ENABLE_PGO requires Clang (IR-based PGO uses "
            "-fprofile-instr-*); current compiler is ${CMAKE_CXX_COMPILER_ID}")
    endif()
    if(PGO_MODE STREQUAL "generate")
        message(STATUS "PGO: INSTRUMENT build (-fprofile-instr-generate)")
        add_compile_options(-fprofile-instr-generate)
        add_link_options(-fprofile-instr-generate)
    elseif(PGO_MODE STREQUAL "use")
        if(NOT EXISTS "${PGO_PROFILE}")
            message(FATAL_ERROR "PGO_MODE=use requires -DPGO_PROFILE=<absolute "
                "path to merged .profdata>; got '${PGO_PROFILE}'")
        endif()
        message(STATUS "PGO: OPTIMIZED build (-fprofile-instr-use=${PGO_PROFILE})")
        # The Release build uses -Werror. In use-mode Clang warns on functions
        # not covered by the profile (HonestBenchmark exercises only the match
        # path, so gateways/research/etc. are "unprofiled") and on profile
        # drift. Demote just those two to warnings so PGO doesn't trip -Werror.
        add_compile_options(-fprofile-instr-use=${PGO_PROFILE}
            -Wno-error=profile-instr-unprofiled
            -Wno-error=profile-instr-out-of-date)
        add_link_options(-fprofile-instr-use=${PGO_PROFILE})
    else()
        message(FATAL_ERROR "ENABLE_PGO=ON requires -DPGO_MODE=generate or "
            "-DPGO_MODE=use")
    endif()
endif()

# Lean Mode Option
option(USE_LEAN_MODE "Build with enterprise features disabled for raw speed" OFF)
if(USE_LEAN_MODE)
    message(STATUS "OB_LEAN_MODE Enabled: Compiling without risk/stats features")
    add_compile_definitions(OB_LEAN_MODE)
endif()

# Capture build metadata for the /version admin endpoint.
# Best-effort — in Docker builds the .git directory is excluded by
# .dockerignore so we fall back to "unknown". A baked-in version
# string still beats no version info at all when someone is trying
# to figure out which binary is running in production.
find_program(GIT_EXECUTABLE git)
if(GIT_EXECUTABLE)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short=12 HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE OB_GIT_SHA_VAR
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE GIT_RESULT
    )
    if(NOT GIT_RESULT EQUAL 0 OR "${OB_GIT_SHA_VAR}" STREQUAL "")
        set(OB_GIT_SHA_VAR "unknown")
    endif()
else()
    set(OB_GIT_SHA_VAR "unknown")
endif()
string(TIMESTAMP OB_BUILD_TIME_VAR "%Y-%m-%dT%H:%M:%SZ" UTC)
add_compile_definitions(
    OB_GIT_SHA="${OB_GIT_SHA_VAR}"
    OB_BUILD_TIME="${OB_BUILD_TIME_VAR}"
)
message(STATUS "Build metadata: git=${OB_GIT_SHA_VAR}, time=${OB_BUILD_TIME_VAR}")

# Threading support
find_package(Threads REQUIRED)

# Include directories
include_directories(include)

# Main Engine Library
add_library(OrderMatcher
    src/AdminServer.cpp
    src/OrderBook.cpp
    src/MatchingEngine.cpp
    src/SessionScheduler.cpp
    src/ContingencyManager.cpp
    src/HierarchicalRiskManager.cpp
    src/EpochStore.cpp
    src/TcpGateway.cpp
    src/FaultInjector.cpp
    src/SimulationDriver.cpp
    src/DpdkGateway.cpp
)
target_link_libraries(OrderMatcher PUBLIC Threads::Threads)

# Optional io_uring-backed journal write path (Linux only). When liburing is
# found we define OB_HAVE_LIBURING (PUBLIC, so the header-only Journal.h sees it
# in every consumer that includes it) and link liburing. If liburing is absent
# — or on any non-Linux platform — this block is a no-op and the journal uses
# the existing fwrite + fdatasync path with identical durability semantics.
# Force off with -DENABLE_IO_URING=OFF.
option(ENABLE_IO_URING "Use io_uring for the journal write path when liburing is available" ON)
if(ENABLE_IO_URING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_library(URING_LIB uring)
    find_path(URING_INCLUDE liburing.h)
    if(URING_LIB AND URING_INCLUDE)
        target_compile_definitions(OrderMatcher PUBLIC OB_HAVE_LIBURING=1)
        target_include_directories(OrderMatcher PUBLIC ${URING_INCLUDE})
        target_link_libraries(OrderMatcher PUBLIC ${URING_LIB})
        message(STATUS "io_uring journal path enabled (liburing: ${URING_LIB})")
    else()
        message(STATUS "io_uring requested but liburing not found — journal uses fwrite + fdatasync")
    endif()
endif()

# Optional DPDK + F-Stack kernel-bypass ingestion path (Linux only). When
# ENABLE_DPDK is on and both libdpdk and F-Stack are found we define OB_HAVE_DPDK
# and link F-Stack (which bundles + manages its own DPDK internally);
# src/DpdkGateway.cpp then compiles its real body. Absent it (any non-Linux
# platform, or DPDK/F-Stack not installed) DpdkGateway.cpp compiles to an empty
# translation unit and the kernel TcpGateway is the only ingestion path.
option(ENABLE_DPDK "Build the DPDK/F-Stack kernel-bypass ingestion path" OFF)
# F-Stack install prefix (meson+ninja default from scripts/dpdk_aws.sh). Its
# static lib lives at ${FF_PATH}/lib/libfstack.a and bundles DPDK internally.
set(FF_PATH "/usr/local" CACHE PATH "F-Stack install prefix (contains lib/libfstack.a)")
if(ENABLE_DPDK AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig REQUIRED)
    # Resolve F-Stack's bundled/patched DPDK, installed under /usr/local by
    # scripts/dpdk_aws.sh (meson+ninja, default prefix) — NOT any distro libdpdk.
    # Prepend its pkgconfig dirs so pkg_check_modules finds the F-Stack DPDK first.
    set(ENV{PKG_CONFIG_PATH}
        "/usr/local/lib/x86_64-linux-gnu/pkgconfig:/usr/local/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
    pkg_check_modules(DPDK REQUIRED libdpdk)  # F-Stack bundled DPDK's libdpdk.pc (static libs only)
    find_library(FSTACK_LIB fstack)          # F-Stack libfstack (/usr/local/lib)
    find_path(FSTACK_INCLUDE ff_api.h)
    if(DPDK_FOUND AND FSTACK_LIB AND FSTACK_INCLUDE)
        target_compile_definitions(OrderMatcher PUBLIC OB_HAVE_DPDK=1)
        target_include_directories(OrderMatcher PUBLIC ${DPDK_INCLUDE_DIRS} ${FSTACK_INCLUDE})
        target_compile_options(OrderMatcher PUBLIC ${DPDK_CFLAGS_OTHER})
        # libfstack.a already has DPDK's object files compiled IN — including the
        # tailq init constructors (e.g. tailqinitfn_rte_swx_ipsec_tailq). So linking
        # ANY librte_*.a alongside it produces a SECOND copy of those same
        # constructors → "EAL: PANIC in tailqinitfn_*: Cannot initialize tailq".
        #
        # Fix: link ONLY libfstack.a statically (inside --whole-archive so its
        # F-Stack + DPDK PMD constructors, incl. the ENA PMD, survive the static
        # link and self-register). Do NOT link any DPDK static lib — resolve the
        # remaining rte_* symbols from the DPDK *shared* libs already installed
        # beside it. pkg_check_modules(DPDK) above is kept ONLY for
        # ${DPDK_INCLUDE_DIRS}/${DPDK_CFLAGS_OTHER}; its static lib list
        # (DPDK_STATIC_LIBRARIES) is deliberately NOT on the link line.
        target_link_libraries(OrderMatcher PUBLIC
            -Wl,--no-as-needed
            -Wl,--whole-archive ${FF_PATH}/lib/libfstack.a -Wl,--no-whole-archive
            # Point the linker at F-Stack's bundled DPDK shared libs so libfstack's
            # rte_* references resolve from librte_*.so — NOT a second static copy.
            # No -lrte_*/librte_*.a on the line; that is the whole point.
            -L${FF_PATH}/lib/x86_64-linux-gnu
            # The ENA PMD uses libibverbs *core* (InfiniBand-free) — plain shared
            # dep. libjitterentropy ships only the versioned .so.3 (no unversioned
            # dev symlink), so link it by exact name.
            -libverbs
            -lz -lzstd -l:libjitterentropy.so.3)
        message(STATUS "DPDK/F-Stack kernel-bypass ingestion enabled (dpdk ${DPDK_VERSION})")
    else()
        message(WARNING "ENABLE_DPDK=ON but libdpdk and/or F-Stack not found — kernel TCP only")
    endif()
endif()

if(ENABLE_ASSERTIONS)
    target_compile_options(OrderMatcher PUBLIC -UNDEBUG)
endif()
if(ENABLE_FAULT_INJECTION)
    # PUBLIC so the macro reaches every consumer (tests, gateway, etc.) and
    # the call sites in headers compile to active fault points.
    target_compile_definitions(OrderMatcher PUBLIC OB_ENABLE_FAULT_INJECTION=1)
    message(STATUS "Fault-injection points enabled (OB_ENABLE_FAULT_INJECTION)")
endif()

# Main Executable
add_executable(OrderEngine src/main.cpp)
target_link_libraries(OrderEngine PRIVATE OrderMatcher)

# Gateway Server
add_executable(GatewayServer src/gateway_main.cpp)
target_link_libraries(GatewayServer PRIVATE OrderMatcher)

# Market Data Subscriber
add_executable(MdSubscriber src/md_subscriber.cpp)
target_link_libraries(MdSubscriber PRIVATE OrderMatcher)

# Testing
option(BUILD_TESTS "Build tests" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Benchmarking
option(BUILD_BENCHMARKS "Build benchmarks" ON)
if(BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

# Fuzzing
option(BUILD_FUZZERS "Build fuzz harnesses (ASan/UBSan replay always; libFuzzer if supported)" OFF)
if(BUILD_FUZZERS)
    add_subdirectory(fuzz)
endif()

# Tools (Replay CLI, etc.)
option(BUILD_TOOLS "Build operational tools" ON)
if(BUILD_TOOLS)
    add_subdirectory(tools)
endif()

