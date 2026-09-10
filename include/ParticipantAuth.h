#pragma once

// ParticipantAuth — bind a wire-supplied ParticipantId to a credential.
//
// THE PROBLEM (audit H7)
//
// Every gateway takes the participant identity straight off the wire and
// trusts it:
//
//   FIX     FIXParser.h — result.participantId = msg.getInt(SenderCompID)
//   OUCH    OuchSession — firmOf_[engineId] = o.firm
//   binary  TcpGateway  — req.participantId, a field in the client's struct
//
// Nothing checks that the sender is entitled to that identity. Anyone who can
// reach the port can:
//
//   * submit orders as any participant, and have the fills and positions land
//     on that participant's account;
//   * evade the per-participant rate limiter by rotating ids, since
//     RateLimiter keys on the same unverified number;
//   * trip another participant's kill switch, or read their state through any
//     participant-scoped surface.
//
// The admin port has had bearer-token auth for a while and refuses to start
// without one unless explicitly opted out. The order-entry ports — the ones
// that move money — have had nothing.
//
// WHAT THIS IS
//
// A credential store and an authorisation check, deliberately separate from
// any protocol. Each gateway authenticates at its own session start (FIX
// Logon, OUCH Login, a binary handshake) and then asks this class one question
// per order: may this session act as this participant?
//
// One credential may cover several participant ids — a firm running multiple
// desks or strategies legitimately submits under more than one — so
// authorisation is set membership, not equality.
//
// WHAT THIS IS NOT
//
// Not a password database. Secrets are supplied by the caller (from config or
// a secret manager) and compared in constant time; this class never persists
// them, and storing them hashed rather than in memory is the caller's job when
// a secret store is wired up. Not a session manager either: gateways own their
// session lifetime and hold the AuthorizedIdentity this returns.
//
// DEFAULT
//
// With no credentials registered, enabled() is false and gateways behave
// exactly as before — unauthenticated. That keeps this change reviewable and
// every existing deployment working. Making authentication REQUIRED by
// default, the way AdminServer already does (refuse to start unless a token is
// set or auth is explicitly disabled), is the necessary follow-up; it is a
// deployment-breaking change and belongs in its own commit rather than being
// smuggled in with the mechanism.

#include "Types.h"

#include <algorithm>
#include <fstream>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <sys/stat.h>

namespace OrderMatcher {

// Timing-safe comparison. A byte-by-byte early return leaks how much of a
// secret is correct through response timing, which recovers it one byte at a
// time. Length is still leaked; that is standard and harmless.
inline bool constantTimeEquals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return diff == 0;
}

// What a gateway holds for the life of an authenticated session. Invalid until
// authenticate() fills it.
class AuthorizedIdentity {
public:
    AuthorizedIdentity() = default;

    bool valid() const { return valid_; }
    const std::string& user() const { return user_; }

    // May this session submit as `id`? False for an unauthenticated identity,
    // so a gateway that forgets to authenticate fails closed rather than open.
    bool permits(ParticipantId id) const {
        if (!valid_) return false;
        return std::find(allowed_.begin(), allowed_.end(), id) != allowed_.end();
    }

    const std::vector<ParticipantId>& allowed() const { return allowed_; }

private:
    friend class ParticipantAuth;
    bool                       valid_{false};
    std::string                user_;
    std::vector<ParticipantId> allowed_;
};

class ParticipantAuth {
public:
    // Register a credential and the participant ids it may act as. Re-adding
    // the same user replaces its entry, so a config reload is idempotent.
    void addCredential(std::string user, std::string secret,
                       std::vector<ParticipantId> allowed) {
        credentials_[user] = Credential{std::move(secret), std::move(allowed)};
    }

    void clear() { credentials_.clear(); }

    // Load credentials from a file, one per line:
    //
    //     # comment
    //     firm-a:s3cret:100,101
    //     firm-b:0th3r:200
    //
    // Deliberately a SEPARATE file rather than a key in engine.conf, following
    // the precedent already set by the admin token (--admin-token /
    // OB_ADMIN_TOKEN, never config): the main config gets copied into tickets
    // and pasted into chat, and secrets should not travel with it. A dedicated
    // file can also carry restrictive permissions, which is checked below.
    //
    // Returns the number of credentials loaded, or -1 on a file that cannot be
    // read. Any malformed line is a hard failure with a description in `error`,
    // not a skip: a typo that silently drops a firm's credential would show up
    // as that firm being unable to trade, at the worst possible moment.
    int loadFromFile(const std::string& path, std::string* error = nullptr) {
        std::ifstream in(path);
        if (!in) {
            if (error) *error = "cannot open credentials file: " + path;
            return -1;
        }

        std::unordered_map<std::string, Credential> loaded;
        std::string line;
        int lineNo = 0;
        while (std::getline(in, line)) {
            ++lineNo;
            const std::string t = trim(line);
            if (t.empty() || t[0] == '#') continue;

            const size_t c1 = t.find(':');
            const size_t c2 = (c1 == std::string::npos) ? std::string::npos
                                                        : t.find(':', c1 + 1);
            if (c1 == std::string::npos || c2 == std::string::npos) {
                if (error) *error = "line " + std::to_string(lineNo) +
                                    ": expected user:secret:ids";
                return -1;
            }

            const std::string user   = trim(t.substr(0, c1));
            const std::string secret = t.substr(c1 + 1, c2 - c1 - 1);
            const std::string idList = t.substr(c2 + 1);
            if (user.empty() || secret.empty()) {
                if (error) *error = "line " + std::to_string(lineNo) +
                                    ": empty user or secret";
                return -1;
            }

            std::vector<ParticipantId> ids;
            size_t pos = 0;
            while (pos <= idList.size()) {
                const size_t comma = idList.find(',', pos);
                const std::string tok =
                    trim(idList.substr(pos, comma == std::string::npos
                                                ? std::string::npos
                                                : comma - pos));
                if (!tok.empty()) {
                    if (tok.find_first_not_of("0123456789") != std::string::npos) {
                        if (error) *error = "line " + std::to_string(lineNo) +
                                            ": non-numeric participant id '" + tok + "'";
                        return -1;
                    }
                    ids.push_back(static_cast<ParticipantId>(std::stoull(tok)));
                }
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
            if (ids.empty()) {
                // Authenticating but authorising nothing is almost certainly a
                // typo, and it fails closed in a way that looks like an engine
                // fault rather than a config one. Say so now.
                if (error) *error = "line " + std::to_string(lineNo) +
                                    ": credential '" + user + "' lists no participant ids";
                return -1;
            }
            loaded[user] = Credential{secret, std::move(ids)};
        }

        credentials_ = std::move(loaded);
        return static_cast<int>(credentials_.size());
    }

    // True if the file is readable by group or other. Secrets in a
    // world-readable file are not secrets; the caller decides whether that is
    // fatal or a warning.
    static bool fileIsOverlyPermissive(const std::string& path) {
        struct stat st{};
        if (::stat(path.c_str(), &st) != 0) return false;
        return (st.st_mode & (S_IRGRP | S_IROTH)) != 0;
    }

    // False when nothing is registered. Gateways skip enforcement entirely in
    // that case, which is the pre-H7 behaviour.
    bool   enabled()          const { return !credentials_.empty(); }
    size_t credentialCount()  const { return credentials_.size(); }

    // Authenticate a session. Returns an invalid identity on any failure —
    // unknown user and wrong secret are deliberately indistinguishable, so a
    // probe cannot enumerate valid users.
    AuthorizedIdentity authenticate(std::string_view user,
                                    std::string_view secret) const {
        AuthorizedIdentity id;
        auto it = credentials_.find(std::string(user));
        if (it == credentials_.end()) {
            // Compare against a fixed dummy anyway, so an unknown user does
            // not return measurably faster than a known one with a bad secret.
            (void)constantTimeEquals(secret, kDummySecret);
            return id;
        }
        if (!constantTimeEquals(secret, it->second.secret)) return id;

        id.valid_   = true;
        id.user_    = it->first;
        id.allowed_ = it->second.allowed;
        return id;
    }

private:
    static std::string trim(const std::string& v) {
        const size_t b = v.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return "";
        const size_t e = v.find_last_not_of(" \t\r\n");
        return v.substr(b, e - b + 1);
    }

    struct Credential {
        std::string                secret;
        std::vector<ParticipantId> allowed;
    };

    // Same length class as a typical secret; only the work done comparing it
    // matters, not the value.
    static constexpr std::string_view kDummySecret = "0000000000000000";

    std::unordered_map<std::string, Credential> credentials_;
};

}  // namespace OrderMatcher
