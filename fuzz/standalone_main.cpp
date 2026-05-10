// Standalone replay driver — runs the libFuzzer entry point against files passed
// on the command line. Lets the harness run on toolchains that don't ship
// libFuzzer (notably Apple Clang on macOS).
//
// Usage: ./fuzz_fix_parser_replay [-q] <file-or-dir> ...
//        -q   Quiet: only report on crash/abort, skip per-file output.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

namespace fs = std::filesystem;

static bool runFile(const fs::path& p, bool quiet) {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "open failed: %s\n", p.c_str());
        return false;
    }
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    if (!quiet) std::printf("replay %s (%zu bytes)\n", p.c_str(), buf.size());
    LLVMFuzzerTestOneInput(buf.data(), buf.size());
    return true;
}

int main(int argc, char** argv) {
    bool quiet = false;
    int first = 1;
    if (argc > 1 && std::strcmp(argv[1], "-q") == 0) { quiet = true; first = 2; }

    if (argc <= first) {
        std::fprintf(stderr,
            "usage: %s [-q] <file-or-dir> ...\n", argv[0]);
        return 2;
    }

    size_t count = 0;
    for (int i = first; i < argc; ++i) {
        fs::path p(argv[i]);
        if (fs::is_directory(p)) {
            for (const auto& entry : fs::recursive_directory_iterator(p)) {
                if (entry.is_regular_file()) {
                    runFile(entry.path(), quiet);
                    ++count;
                }
            }
        } else if (fs::is_regular_file(p)) {
            runFile(p, quiet);
            ++count;
        }
    }
    std::printf("replayed %zu inputs\n", count);
    return 0;
}
