#pragma once
// JournalBoot — the one boot-time journal sequence every binary runs.
//
// Enabling, refusing and replaying a journal is safety-critical, and this
// codebase's most repeated defect is drift between hand-copied variants of the
// same logic. So the engine binary and the order-entry binary share this
// function instead of each carrying its own copy of it.

#include "CliFlags.h"
#include "Journal.h"
#include "MatchingEngine.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>

namespace OrderMatcher {

// Enables the journal named by --journal / OB_JOURNAL_PATH, refuses what must be
// refused, and replays the rest into `engine`. Returns 0 to carry on booting, or
// the exit code the process should return. With no journal configured it does
// nothing and returns 0.
//
// Must run before any worker starts and before any listener accepts. Replay
// drives the books directly rather than through the submit path, so a worker or
// an admitted order running alongside it would race it — and being ahead of both
// is also why replay cannot re-journal what it applies: it never reaches the
// code that writes entries.
//
// `policy` is when appends reach the disk (see MatchingEngine::enableJournal):
// the order-entry binary passes Immediate so that its acks are durable.
inline int bootJournal(MatchingEngine& engine, int argc, char** argv,
                       Journal::SyncPolicy policy = Journal::SyncPolicy::GroupCommit) {
    const std::string journalPath = flagOrEnv(argc, argv, "--journal", "OB_JOURNAL_PATH");
    if (journalPath.empty()) return 0;

    // A journal we cannot safely append to is not the same as no journal.
    // Starting anyway would serve an empty or partial book while the real
    // resting orders sit on disk, or append where replay can never reach — and
    // the operator would have no reason to suspect it, because a book missing
    // orders looks like any other book. The Journal prints what is wrong and
    // how to repair it; refusing to boot is what makes someone read it.
    if (!engine.enableJournal(journalPath, policy)) {
        std::cerr << "[Engine] FATAL: journal at " << journalPath
                  << " could not be opened safely (see the [Journal] message above).\n"
                  << "        Refusing to start. Follow the repair it gives, move the\n"
                  << "        file aside to start fresh, or replay it with the build\n"
                  << "        that wrote it.\n";
        return 1;
    }
    std::cout << "[Engine] Journal enabled at " << journalPath << "\n";

    // RECOVER. The engine binary used to write a write-ahead log and never read
    // it back: replayJournal() existed, was covered by tests, and was called by
    // tools/JournalReplayCLI and nothing else. Every restart opened an EMPTY BOOK
    // while the resting orders sat on disk — and, as the refusal above says
    // about an unreadable journal, nothing about the running system told anyone.
    const size_t recoverable = engine.getJournal()
                             ? engine.getJournal()->strictPrefixEntries() : 0;

    // REFUSE A JOURNAL THAT MAY HOLD ORDERS NOBODY SENT.
    //
    // Until cdd45ac the engine binary seeded 200 synthetic Limit orders per
    // symbol at boot, as participants 1 and 2 — and enableJournal ran BEFORE that
    // loop, so every journal such a build wrote holds them. Replaying one
    // restores those orders and reports "Replayed 400 of 400" while doing it.
    // Reproduced on a journal written by the 1bfa4fc binary: 10 bid and 10 ask
    // levels on both symbols, liquidity no client submitted, ready to be traded
    // against.
    //
    // This is a content judgement, not a read error — the records frame
    // perfectly — so it is checked here rather than by setting recoveryFailed_,
    // which would also block appending and offer no way out.
    //
    // The override exists because an operator upgrading a node with a live
    // journal has a real decision, and it has to be theirs: once replayed, the
    // synthetic orders are indistinguishable from real ones, so nothing here can
    // clean them up afterwards.
    const bool replayLegacy = flagOrEnvBool(argc, argv, "--replay-legacy-journal",
                                            "OB_REPLAY_LEGACY_JOURNAL");
    const uint64_t epoch = engine.getJournal() ? engine.getJournal()->contentEpoch()
                                               : JOURNAL_EPOCH_NO_SEEDING;
    if (recoverable > 0 && epoch == JOURNAL_EPOCH_LEGACY && !replayLegacy) {
        std::cerr << "[Engine] FATAL: " << journalPath << " may hold orders nobody sent\n"
                  << "        (journal content epoch " << epoch << "; this build writes "
                  << JOURNAL_EPOCH_NO_SEEDING << "). Its\n"
                  << "        lineage began with a build that seeded synthetic orders at startup:\n"
                  << "        either that build wrote it, or it is a checkpoint of one replayed\n"
                  << "        under --replay-legacy-journal (checkpoints inherit the epoch).\n"
                  << "        It holds " << recoverable << " recoverable entries, and replaying them\n"
                  << "        could rest up to 200 orders per symbol for participants 1 and 2\n"
                  << "        that no client sent. Clients would trade against them.\n"
                  << "        Inspect it:  JournalReplayCLI --journal " << journalPath
                  << " --stats\n"
                  << "        Start clean: move the file aside.\n"
                  << "        Replay anyway, having decided the contents are real:\n"
                  << "        --replay-legacy-journal (OB_REPLAY_LEGACY_JOURNAL=1).\n";
        return 1;
    }
    if (replayLegacy && epoch == JOURNAL_EPOCH_LEGACY) {
        std::cout << "[Engine] WARNING: replaying a legacy journal on request — it may\n"
                     "         hold synthetic startup orders for participants 1 and 2.\n"
                     "         It stays marked legacy through every checkpoint, so this\n"
                     "         flag will be needed on each boot of it: nothing can certify\n"
                     "         the book clean once synthetic and real orders are mixed.\n";
    }

    const size_t replayed = engine.replayJournal();

    if (recoverable > 0 && replayed == 0) {
        std::cerr << "[Engine] FATAL: journal holds " << recoverable
                  << " recoverable entries but replay applied none.\n"
                  << "        Refusing to start with an empty book on top of a\n"
                  << "        journal that has state in it. Inspect it with\n"
                  << "        JournalReplayCLI --journal " << journalPath
                  << " --stats\n";
        return 1;
    }
    std::cout << "[Engine] Replayed " << replayed << " of " << recoverable
              << " recoverable journal entries\n";
    if (replayed != recoverable) {
        std::cout << "[Engine] NOTE: counts differ — entries at or below an\n"
                     "         already-seen sequence number are skipped.\n";
    }

    // WHAT THIS STILL DOES NOT CATCH, so nobody reads the line above as more than
    // it is: replayJournal counts entries CONSUMED, not entries applied. The
    // dispatch discards every return value (addOrder and modifyOrder in
    // MatchingEngine::replayJournal), so an entry that replayed and was REFUSED —
    // a duplicate order id, an exhausted pool, or a quantity that a newer guard
    // now rejects — is counted as replayed and silently dropped.
    // `replayed == recoverable` therefore does not mean the book matches the one
    // that was journaled. Closing that needs replay to check its own results
    // against an integrity oracle that can see a wrong book, which
    // validateIntegrity() currently cannot.
    //
    // Per-participant kill cancels are also not journaled (THR-2), so an operator
    // must re-issue any kill after a restart. The runbook says so.
    return 0;
}

}  // namespace OrderMatcher
