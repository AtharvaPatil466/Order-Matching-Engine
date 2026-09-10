// ParticipantAuthTest — audit H7: a wire-supplied ParticipantId must be backed
// by a credential.
//
// Before this, every gateway trusted the identity in the message:
//
//   FIX     result.participantId = msg.getInt(SenderCompID)
//   OUCH    firmOf_[engineId] = o.firm
//   binary  req.participantId, a field in the client's own struct
//
// So anyone who could reach the port could trade as any participant, evade the
// per-participant rate limiter by rotating ids, and reach participant-scoped
// controls including the kill switch.
//
// Distinct from AdminAuthTest, which covers the admin port's bearer token.

#include "ParticipantAuth.h"
#include "SoupBinTCP.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

using namespace OrderMatcher;

namespace {

int passed = 0;
#define TEST(name) std::cout << "  " << #name << "..." << std::flush
#define PASS() do { std::cout << " ok" << std::endl; ++passed; } while (0)

// ─── 1: disabled until credentials exist ────────────────────────────────────
//
// With nothing registered the gateways skip enforcement entirely, which is the
// pre-H7 behaviour. That is what keeps this change reviewable; making auth
// REQUIRED is a separate, deployment-breaking commit.
void test_DisabledWithNoCredentials() {
    TEST(DisabledWithNoCredentials);
    ParticipantAuth auth;
    assert(!auth.enabled());
    assert(auth.credentialCount() == 0);
    PASS();
}

// ─── 2: a good credential authorises exactly its own participants ───────────
void test_AuthorizesOnlyItsOwnParticipants() {
    TEST(AuthorizesOnlyItsOwnParticipants);
    ParticipantAuth auth;
    auth.addCredential("firm-a", "s3cret", {100, 101});
    assert(auth.enabled());

    auto id = auth.authenticate("firm-a", "s3cret");
    assert(id.valid());
    assert(id.user() == "firm-a");

    // One credential covering several ids is the point: a firm running two
    // desks legitimately submits under both.
    assert(id.permits(100));
    assert(id.permits(101));

    // And nothing else. This is the whole vulnerability: 200 belongs to
    // someone else, and claiming it in tag 49 must not be enough.
    assert(!id.permits(200));
    assert(!id.permits(0));
    PASS();
}

// ─── 3: bad secret and unknown user both fail ───────────────────────────────
void test_RejectsBadSecretAndUnknownUser() {
    TEST(RejectsBadSecretAndUnknownUser);
    ParticipantAuth auth;
    auth.addCredential("firm-a", "s3cret", {100});

    assert(!auth.authenticate("firm-a", "wrong").valid());
    assert(!auth.authenticate("firm-a", "").valid());
    assert(!auth.authenticate("nobody", "s3cret").valid());
    // A prefix of the real secret must not pass — the comparison is over the
    // whole value, not a startswith.
    assert(!auth.authenticate("firm-a", "s3cre").valid());
    PASS();
}

// ─── 4: an unauthenticated identity permits nothing ─────────────────────────
//
// Fail closed. A gateway that forgets to authenticate, or one holding the
// default-constructed identity after a failed logon, must not submit as anyone.
void test_DefaultIdentityPermitsNothing() {
    TEST(DefaultIdentityPermitsNothing);
    AuthorizedIdentity id;
    assert(!id.valid());
    assert(!id.permits(0));
    assert(!id.permits(100));
    assert(id.allowed().empty());
    PASS();
}

// ─── 5: re-adding a user replaces it, so config reload is idempotent ────────
void test_ReAddingCredentialReplaces() {
    TEST(ReAddingCredentialReplaces);
    ParticipantAuth auth;
    auth.addCredential("firm-a", "old", {100});
    auth.addCredential("firm-a", "new", {200});

    assert(auth.credentialCount() == 1);
    assert(!auth.authenticate("firm-a", "old").valid() && "rotated secret must stop working");

    auto id = auth.authenticate("firm-a", "new");
    assert(id.valid());
    assert(id.permits(200));
    assert(!id.permits(100) && "revoked participant must stop being permitted");
    PASS();
}

// ─── 6: how much of the secret is right changes nothing ─────────────────────
//
// Not a timing measurement — those are far too noisy to assert on, and a
// throughput floor inside a correctness test is what made other tests in this
// repo flaky. This pins the observable property the constant-time comparison
// exists for.
void test_WrongSecretsAllFailRegardlessOfPrefix() {
    TEST(WrongSecretsAllFailRegardlessOfPrefix);
    ParticipantAuth auth;
    const std::string secret = "abcdefghijklmnop";
    auth.addCredential("firm-a", secret, {100});

    for (size_t n = 0; n < secret.size(); ++n) {
        std::string guess = secret.substr(0, n) + std::string(secret.size() - n, 'X');
        assert(!auth.authenticate("firm-a", guess).valid());
    }
    assert(auth.authenticate("firm-a", secret).valid() && "the real secret still works");
    PASS();
}

// ─── 7: constantTimeEquals itself ───────────────────────────────────────────
void test_ConstantTimeEqualsBasics() {
    TEST(ConstantTimeEqualsBasics);
    assert(constantTimeEquals("", ""));
    assert(constantTimeEquals("abc", "abc"));
    assert(!constantTimeEquals("abc", "abd"));
    assert(!constantTimeEquals("abc", "ab"));    // length differs
    assert(!constantTimeEquals("abc", "abcd"));
    PASS();
}

// ─── 8: the OUCH gateway had the reject code all along ──────────────────────
//
// SOUP_LOGIN_REJECT_NOT_AUTHORIZED ('A') has been defined since the SoupBinTCP
// header was written. Nothing ever sent it, because nothing ever checked a
// credential — the login validator defaulted to "accept every login".
void test_SoupNotAuthorizedCodeExists() {
    TEST(SoupNotAuthorizedCodeExists);
    assert(SOUP_LOGIN_REJECT_NOT_AUTHORIZED == 'A');
    assert(SOUP_LOGIN_REJECT_SESSION_NOT_AVAILABLE == 'S');
    PASS();
}

// ─── 9: a credential covering no participants authorises nothing ────────────
//
// A misconfiguration — credential present, allow-list empty — must fail
// closed. Authenticating successfully is not the same as being entitled to
// anything.
void test_EmptyAllowListAuthorizesNothing() {
    TEST(EmptyAllowListAuthorizesNothing);
    ParticipantAuth auth;
    auth.addCredential("firm-a", "s3cret", {});

    auto id = auth.authenticate("firm-a", "s3cret");
    assert(id.valid() && "the credential itself is good");
    assert(!id.permits(0));
    assert(!id.permits(100) && "authenticated is not the same as authorised");
    PASS();
}

// ─── 10: loading credentials from a file ────────────────────────────────────
void test_LoadsCredentialsFromFile() {
    TEST(LoadsCredentialsFromFile);
    const std::string path = "/tmp/ob_creds_ok.txt";
    {
        std::ofstream out(path);
        out << "# desks for firm-a\n"
            << "firm-a:s3cret:100,101\n"
            << "\n"
            << "firm-b:0th3r:200\n";
    }

    ParticipantAuth auth;
    std::string err;
    assert(auth.loadFromFile(path, &err) == 2 && err.empty());

    auto a = auth.authenticate("firm-a", "s3cret");
    assert(a.valid() && a.permits(100) && a.permits(101) && !a.permits(200));
    auto b = auth.authenticate("firm-b", "0th3r");
    assert(b.valid() && b.permits(200) && !b.permits(100));

    std::remove(path.c_str());
    PASS();
}

// ─── 11: a malformed line fails loudly, it does not skip ────────────────────
//
// Skipping would drop a firm's credential silently, and the symptom — that
// firm unable to trade — would surface at the worst possible moment and look
// like an engine fault rather than a typo.
void test_MalformedCredentialLineIsFatal() {
    TEST(MalformedCredentialLineIsFatal);
    struct Case { const char* body; const char* what; };
    const Case cases[] = {
        {"firm-a:s3cret\n",           "missing id list"},
        {"firm-a:s3cret:1,abc\n",     "non-numeric id"},
        {":s3cret:100\n",             "empty user"},
        {"firm-a::100\n",             "empty secret"},
        {"firm-a:s3cret:\n",          "no ids at all"},
    };

    for (const auto& c : cases) {
        const std::string path = "/tmp/ob_creds_bad.txt";
        { std::ofstream out(path); out << c.body; }

        ParticipantAuth auth;
        std::string err;
        const int n = auth.loadFromFile(path, &err);
        assert(n == -1 && "malformed line must fail the load");
        assert(!err.empty() && "and must say which line and why");
        assert(!auth.enabled() && "a failed load must not half-apply");
        std::remove(path.c_str());
    }
    PASS();
}

// ─── 12: a missing file is an error, not silently "no credentials" ──────────
//
// Treating an unreadable file as "auth disabled" would turn a typo in the path
// into a silently unauthenticated venue.
void test_MissingCredentialFileIsAnError() {
    TEST(MissingCredentialFileIsAnError);
    ParticipantAuth auth;
    std::string err;
    assert(auth.loadFromFile("/tmp/ob_creds_does_not_exist_12345", &err) == -1);
    assert(!err.empty());
    assert(!auth.enabled());
    PASS();
}

}  // namespace

int main() {
    std::cout << "\n=== Participant Auth Tests (H7) ===\n\n";

    test_DisabledWithNoCredentials();
    test_AuthorizesOnlyItsOwnParticipants();
    test_RejectsBadSecretAndUnknownUser();
    test_DefaultIdentityPermitsNothing();
    test_ReAddingCredentialReplaces();
    test_WrongSecretsAllFailRegardlessOfPrefix();
    test_ConstantTimeEqualsBasics();
    test_SoupNotAuthorizedCodeExists();
    test_EmptyAllowListAuthorizesNothing();
    test_LoadsCredentialsFromFile();
    test_MalformedCredentialLineIsFatal();
    test_MissingCredentialFileIsAnError();

    std::cout << "\n" << passed << " passed\n\n";
    return 0;
}
