#pragma once

#include <ydb/core/tx/schemeshard/schemeshard_path_footprint.h>

#include <util/generic/string.h>
#include <util/generic/strbuf.h>
#include <util/generic/vector.h>
#include <util/system/mutex.h>

#include <utility>

namespace NSchemeShardUT_Private {

////////////////////////////////////////////////////////////////////////////////
// Read-set coverage gate.
//
// Every path a part's Propose() resolves must be something the part's
// TPathFootprint already accounts for. A Propose that reads a path the
// footprint never names means the footprint understates that operation, and
// any consumer of the footprint (audit, canonicalization, relocation) would
// silently miss it.
//
// The gate is an IPathFootprintObserver: TTestEnv installs one by default, so
// every SchemeShard unit test asserts coverage without a per-test edit. It
// accumulates violations on the tablet's actor thread and TTestEnv reports
// them from its destructor, on the unittest thread, as a non-fatal failure.

// Is `path` the same path as `prefix`, or a path under it? Segment-wise, so
// "/MyRoot/Tab" is not under "/MyRoot/Table".
bool IsAtOrUnderPath(TStringBuf path, TStringBuf prefix);

// The coverage predicate for one recorded read. A read is covered when it is:
//   1. a path id the part wrote or republished;
//   2. an entry of the footprint, or an ancestor of one (resolving /a/b/c
//      walks /a and /a/b);
//   3. inside a subtree an Implicit entry declared runtime-derived;
//   4. the working dir or an ancestor of it, which every check chain walks;
//   5. the root, or a walk that resolved nothing, which say nothing about
//      scope;
//   6. covered by a documented allowlist entry (see the .cpp).
//   7. an entry path declared by an *earlier* part of the same transaction,
//      or an ancestor of one. TPath::ResolveWithInactive walks the target of
//      every earlier sub-operation of the transaction, so a Move part reads
//      its siblings' destinations; those are named by the request, just not by
//      this part. `earlierInTx` carries those paths; pass an empty span to
//      check one footprint in isolation.
bool IsReadCovered(const NKikimr::NSchemeShard::TPathFootprint& footprint,
    const NKikimr::NSchemeShard::TPathRead& read,
    const TVector<TString>& earlierInTx = {});

// One rendered line per uncovered read of this footprint. Empty when clean.
TVector<TString> ReadSetViolations(const NKikimr::NSchemeShard::TPathFootprint& footprint,
    const TVector<TString>& earlierInTx = {});

// Set YDB_SCHEMESHARD_READSET_GATE=0 to turn the gate off process-wide.
bool ReadSetGateEnabledInEnv();

class TReadSetGate: public NKikimr::NSchemeShard::IPathFootprintObserver {
public:
    // `next`, when set, receives every callback after the gate has checked it.
    explicit TReadSetGate(IPathFootprintObserver* next = nullptr)
        : Next(next)
    {}

    void OnRequestFootprint(NKikimr::NSchemeShard::TTxId txId,
        const NKikimr::NSchemeShard::TPathFootprint& footprint) override;
    void OnPartFootprint(NKikimr::NSchemeShard::TTxId txId,
        const NKikimr::NSchemeShard::TPathFootprint& footprint) override;

    // The gate always wants the read set; a chained observer that wants it too
    // is served by the same recording.
    bool WantReadSet() const override {
        return true;
    }

    // Deduplicated violation lines, in first-seen order.
    TVector<TString> Violations() const;

    // A broken operation can read the same wrong path on every one of
    // thousands of parts; keep the report readable and bounded.
    static constexpr size_t MaxViolations = 32;

private:
    // Entry paths declared so far by the request footprint and the earlier
    // parts of one transaction. Bounded: only the few most recent transactions
    // are kept, which is all ResolveWithInactive can reach.
    TVector<TString>& TxPathsFor(NKikimr::NSchemeShard::TTxId txId);

    static constexpr size_t MaxTrackedTransactions = 8;

    IPathFootprintObserver* const Next = nullptr;
    mutable TMutex Lock;
    TVector<TString> Collected;
    // Guarded by Lock, like Collected: parts of one transaction all arrive on
    // one tablet thread, but several tablets share one gate.
    TVector<std::pair<NKikimr::NSchemeShard::TTxId, TVector<TString>>> TxPaths;
};

}
