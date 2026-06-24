cmake_minimum_required(VERSION 3.15)
project(bcs_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(pybind11 CONFIG REQUIRED)
find_package(Threads REQUIRED)

# Repo root is two levels up from bcs_research/bindings/.
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# Link against the prebuilt engine static library. Overridable via -D.
set(ORDERMATCHER_LIB "${REPO_ROOT}/build/libOrderMatcher.a"
    CACHE FILEPATH "Path to the prebuilt OrderMatcher static library")
if(NOT EXISTS "${ORDERMATCHER_LIB}")
    message(FATAL_ERROR
        "Engine static library not found at ${ORDERMATCHER_LIB}.\n"
        "Build the engine first:  cmake -S . -B build && cmake --build build -j")
endif()

pybind11_add_module(bcs_engine engine_bindings.cpp)
target_include_directories(bcs_engine PRIVATE "${REPO_ROOT}/include")
target_link_libraries(bcs_engine PRIVATE "${ORDERMATCHER_LIB}" Threads::Threads)

# CRITICAL — keep assertions ON to match the engine library.
# The root build compiles OrderMatcher with -UNDEBUG (ENABLE_ASSERTIONS=ON),
# so the engine's many inline header functions are compiled with assert()
# active. A Release build of this module would define NDEBUG and compile those
# SAME inline functions with assert() stripped — an ODR violation across the
# .a / module boundary that manifests as a crash on engine teardown. -UNDEBUG
# (applied after Release's -DNDEBUG) re-enables assertions so both sides agree.
target_compile_options(bcs_engine PRIVATE -UNDEBUG)
