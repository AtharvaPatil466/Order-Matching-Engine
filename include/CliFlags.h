#pragma once

// Command-line and environment lookup shared by the engine and gateway
// binaries. Both follow the same precedence — an explicit flag wins, then the
// environment variable, then the default — and both now have a security
// default that refuses to start, so the two had better agree on what "the
// operator opted out" looks like.

#include <cstdlib>
#include <cstring>
#include <string>

namespace OrderMatcher {

// A flag that takes a value: `--flag value`, else $env, else empty.
inline std::string flagOrEnv(int argc, char** argv, const char* flag,
                             const char* env) {
    for (int i = 1; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], flag) == 0) {
            return argv[i + 1];
        }
    }
    if (const char* v = std::getenv(env)) return v;
    return {};
}

// A boolean flag. Present on its own is enough: `--no-participant-auth` with
// nothing after it is what an operator actually types, and the value form
// above cannot see a flag sitting in the last argv slot — so a bare flag read
// that way silently did nothing. The env form still takes 1/true.
inline bool flagOrEnvBool(int argc, char** argv, const char* flag,
                          const char* env) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], flag) == 0) return true;
    }
    const char* v = std::getenv(env);
    return v && (std::strcmp(v, "1") == 0 || std::strcmp(v, "true") == 0);
}

}  // namespace OrderMatcher
