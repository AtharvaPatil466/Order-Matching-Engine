#pragma once
// A temp file path no other running process shares.
//
// ctest -j runs suites side by side, gtest_discover_tests runs every case in
// its own process, and sibling worktrees run the same binaries at the same
// time. A fixed "/tmp/x.journal" let one run truncate, append to or replay
// another's file. The pid separates every live process; each caller still
// removes its file first, because a dead process's pid can come back.
//
// temp_directory_path() honours $TMPDIR, like the shell tests'
// ${TMPDIR:-/tmp}/name_$$.

#include <filesystem>
#include <string>
#include <unistd.h>

inline std::string uniqueTempPath(const std::string& name) {
    return (std::filesystem::temp_directory_path() /
            ("ob" + std::to_string(::getpid()) + "_" + name))
        .string();
}
