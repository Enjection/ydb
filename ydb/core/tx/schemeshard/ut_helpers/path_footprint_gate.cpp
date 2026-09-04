#include "path_footprint_gate.h"

#include <array>

#include <util/generic/algorithm.h>
#include <util/generic/hash_set.h>
#include <util/string/builder.h>
#include <util/string/cast.h>
#include <util/system/env.h>

namespace NSchemeShardUT_Private {

using namespace NKikimr;
using namespace NKikimr::NSchemeShard;

namespace {

////////////////////////////////////////////////////////////////////////////////
// Allowlist.
//
// A read that no footprint field can ever name, because the Propose reads a
// path the request does not mention and does not write either. Every entry
// must cite the Propose line that makes the read, and must be as narrow as the
// shape allows. An empty list is the goal.

struct TAllowedRead {
    // The op type of the part whose Propose made the read.
    NKikimrSchemeOp::EOperationType PartOpType;
    // Predicate on the read itself.
    bool (*Matches)(const TPathFootprint& footprint, const TPathRead& read);
    // Why this read is legitimate, and where it is made.
    const char* Why;
};

// Keep this list as short as the truth allows: each entry is a read the
// footprint genuinely cannot describe.
bool IsBackupCollectionsDir(const TPathFootprint&, const TPathRead& read) {
    return read.AbsPath.EndsWith("/.backups/collections");
}

constexpr std::array<TAllowedRead, 4> ALLOWED_READS{{
    // Every backup-collection Propose resolves the domain's fixed
    // ".backups/collections" directory to check that the collection lands
    // there (schemeshard__backup_collection_common.cpp:59, :81). It is derived
    // from the domain path, so no proto field of the request names it. A
    // well-formed request's Name entry sits under that directory, which covers
    // the read by rule 2; this entry only fires for a request whose Name points
    // elsewhere and which the same Propose then rejects.
    {NKikimrSchemeOp::ESchemeOpCreateBackupCollection, &IsBackupCollectionsDir,
        "the fixed <domain>/.backups/collections directory"},
    {NKikimrSchemeOp::ESchemeOpDropBackupCollection, &IsBackupCollectionsDir,
        "the fixed <domain>/.backups/collections directory"},
    {NKikimrSchemeOp::ESchemeOpAlterBackupCollection, &IsBackupCollectionsDir,
        "the fixed <domain>/.backups/collections directory"},
    {NKikimrSchemeOp::ESchemeOpBackupBackupCollection, &IsBackupCollectionsDir,
        "the fixed <domain>/.backups/collections directory"},
}};

bool IsAllowlisted(const TPathFootprint& footprint, const TPathRead& read) {
    for (const auto& allowed : ALLOWED_READS) {
        if (allowed.PartOpType == footprint.PartOpType && allowed.Matches(footprint, read)) {
            return true;
        }
    }
    return false;
}

TString RenderEntries(const TPathFootprint& footprint) {
    if (footprint.Entries.empty()) {
        return "<none>";
    }
    TStringBuilder rendered;
    for (const auto& entry : footprint.Entries) {
        if (!rendered.empty()) {
            rendered << ", ";
        }
        rendered << entry.Ref.FieldPath << '=' << entry.AbsPath
                 << '[' << PathRefKindName(entry.Ref.Kind) << ']';
    }
    return rendered;
}

}

bool IsAtOrUnderPath(TStringBuf path, TStringBuf prefix) {
    if (path.empty() || prefix.empty()) {
        return false;
    }
    if (prefix == "/") {
        return true;
    }
    if (!path.StartsWith(prefix)) {
        return false;
    }
    return path.size() == prefix.size() || path[prefix.size()] == '/';
}

bool IsReadCovered(const TPathFootprint& footprint, const TPathRead& read,
        const TVector<TString>& earlierInTx) {
    if (read.AbsPath.empty() || read.AbsPath == "/") {
        // A walk that resolved nothing, or the root, says nothing about scope.
        return true;
    }
    // 1. Something the part actually wrote or republished. Compared by path id
    //    because the write set carries ids, not strings.
    if (read.Resolved) {
        if (Find(footprint.WriteSet, read.PathId) != footprint.WriteSet.end()
                || Find(footprint.Published, read.PathId) != footprint.Published.end()) {
            return true;
        }
    }
    for (const auto& entry : footprint.Entries) {
        if (entry.AbsPath.empty()) {
            continue;
        }
        // 2. An entry, or an ancestor of one: resolving /a/b/c walks /a and /a/b.
        if (IsAtOrUnderPath(entry.AbsPath, read.AbsPath)) {
            return true;
        }
        // 3. Inside a subtree the footprint declared runtime-derived.
        if (entry.Ref.Kind == EPathRefKind::Implicit
                && IsAtOrUnderPath(read.AbsPath, entry.AbsPath)) {
            return true;
        }
    }
    // 4. The working dir or an ancestor of it, which every check chain walks.
    if (IsAtOrUnderPath(footprint.WorkingDirCanon, read.AbsPath)) {
        return true;
    }
    // 7. A path an earlier part of the same transaction declared, or an
    //    ancestor of one. TPath::ResolveWithInactive (schemeshard_path.cpp:
    //    1500-1530) calls TPath::Init on the TargetPathId of every earlier
    //    sub-operation of this transaction, so a Move* part reads the
    //    destinations its sibling parts created. The request names them; this
    //    part does not.
    for (const TString& earlier : earlierInTx) {
        if (IsAtOrUnderPath(earlier, read.AbsPath)) {
            return true;
        }
    }
    return IsAllowlisted(footprint, read);
}

TVector<TString> ReadSetViolations(const TPathFootprint& footprint,
        const TVector<TString>& earlierInTx) {
    TVector<TString> violations;
    for (const auto& read : footprint.ReadSet) {
        if (IsReadCovered(footprint, read, earlierInTx)) {
            continue;
        }
        violations.push_back(TStringBuilder()
            << NKikimrSchemeOp::EOperationType_Name(footprint.PartOpType)
            << " (workingDir " << footprint.WorkingDirCanon << ")"
            << " read " << read.AbsPath
            << (read.Resolved ? " [resolved]" : " [unresolved]")
            << (read.ByPathId ? " [byPathId]" : "")
            << "; entries: " << RenderEntries(footprint));
    }
    return violations;
}

bool ReadSetGateEnabledInEnv() {
    const TString value = GetEnv("YDB_SCHEMESHARD_READSET_GATE");
    return value != "0";
}

TVector<TString>& TReadSetGate::TxPathsFor(TTxId txId) {
    for (auto& [id, paths] : TxPaths) {
        if (id == txId) {
            return paths;
        }
    }
    if (TxPaths.size() >= MaxTrackedTransactions) {
        TxPaths.erase(TxPaths.begin());
    }
    TxPaths.emplace_back(txId, TVector<TString>());
    return TxPaths.back().second;
}

void TReadSetGate::OnRequestFootprint(TTxId txId, const TPathFootprint& footprint) {
    // The request layer is resolved before any part runs, so it carries no
    // read set. Its entries do count towards what the transaction named.
    {
        TGuard<TMutex> guard(Lock);
        TVector<TString>& paths = TxPathsFor(txId);
        for (const auto& entry : footprint.Entries) {
            if (!entry.AbsPath.empty()) {
                paths.push_back(entry.AbsPath);
            }
        }
    }
    if (Next) {
        Next->OnRequestFootprint(txId, footprint);
    }
}

void TReadSetGate::OnPartFootprint(TTxId txId, const TPathFootprint& footprint) {
    // Checked here, per part, so the offending part is named even when a later
    // part of the same request is clean. Only paths named *before* this part
    // count, which is exactly what ResolveWithInactive can reach.
    {
        TGuard<TMutex> guard(Lock);
        TVector<TString>& paths = TxPathsFor(txId);
        for (const TString& violation : ReadSetViolations(footprint, paths)) {
            if (Collected.size() >= MaxViolations) {
                break;
            }
            const TString line = TStringBuilder() << "txId " << ui64(txId) << ": " << violation;
            if (Find(Collected, line) == Collected.end()) {
                Collected.push_back(line);
            }
        }
        for (const auto& entry : footprint.Entries) {
            if (!entry.AbsPath.empty()) {
                paths.push_back(entry.AbsPath);
            }
        }
    }
    if (Next) {
        Next->OnPartFootprint(txId, footprint);
    }
}

TVector<TString> TReadSetGate::Violations() const {
    TGuard<TMutex> guard(Lock);
    return Collected;
}

}
