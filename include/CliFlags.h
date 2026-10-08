#pragma once

// Command-line and environment lookup shared by the engine and gateway
// binaries. Both follow the same precedence — an explicit flag wins, then the
// environment variable, then the default — and both now have a security
// default that refuses to start, so the two had better agree on what "the
// operator opted out" looks like.

#include <cstdlib>
#include <cstring>
#include <ostream>
#include <span>
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

// One flag a binary accepts. `value` names its argument ("PATH", "N");
// nullptr makes it a boolean that takes none.
struct CliFlag {
    const char* name;
    const char* value;
    const char* help;
};

// What is wrong with argv[first..], or "" when every argument is a known flag
// followed by its value if it takes one.
//
// Both binaries used to look each flag up by name and skip everything else, so
// a misspelt or retired flag was dropped without a word and the process started
// without it (DOC-3: the Runbook's `--lean` and `--replay-only` did nothing). An
// ignored flag looks exactly like one that took effect, so neither binary may
// start on an argument it does not understand.
inline std::string cliArgsError(int argc, char** argv, std::span<const CliFlag> known,
                                int first = 1) {
    for (int i = first; i < argc; ++i) {
        const CliFlag* flag = nullptr;
        for (const CliFlag& k : known) {
            if (std::strcmp(argv[i], k.name) == 0) { flag = &k; break; }
        }
        if (!flag) return std::string("unknown argument '") + argv[i] + "'";
        if (flag->value && ++i >= argc) return std::string(flag->name) + " needs a value";
    }
    return {};
}

// One line per flag, for --help, from the same table cliArgsError checks.
inline void printCliFlags(std::ostream& out, std::span<const CliFlag> known) {
    for (const CliFlag& k : known) {
        std::string lhs = std::string("  ") + k.name;
        if (k.value) lhs += std::string(" ") + k.value;
        out << lhs;
        if (lhs.size() < 34) out << std::string(34 - lhs.size(), ' ');
        else out << "\n" << std::string(34, ' ');
        out << k.help << "\n";
    }
}

}  // namespace OrderMatcher
