#pragma once

#include "schemeshard_identificators.h"
#include "schemeshard_info_types.h"
#include "schemeshard_path_element.h"

#include <ydb/core/tx/schemeshard/olap/table/table.h>

#include <util/generic/ptr.h>
#include <util/generic/stack.h>
#include <util/generic/vector.h>

#include <optional>

namespace NKikimr::NSchemeShard {

class TSchemeShard;

// Undo log slot: a LIFO stack for UnDo(), a forward range for observers.
// Same push/top/pop/empty semantics as the TStack it replaces; the only
// difference is that the entries can be walked without popping them, which is
// what lets a caller diff the log across a single Propose().
template <class T>
class TUndoStack {
    TVector<T> Items;

public:
    template <class... TArgs>
    void emplace(TArgs&&... args) {
        Items.emplace_back(std::forward<TArgs>(args)...);
    }

    const T& top() const {
        return Items.back();
    }

    void pop() {
        Items.pop_back();
    }

    size_t size() const {
        return Items.size();
    }

    explicit operator bool() const noexcept {
        return !Items.empty();
    }

    auto begin() const {
        return Items.begin();
    }

    auto end() const {
        return Items.end();
    }
};

class TMemoryChanges: public TSimpleRefCount<TMemoryChanges> {
    using TPathState = std::pair<TPathId, TPathElement::TPtr>;
    TUndoStack<TPathState> Paths;

    using TIndexState = std::pair<TPathId, TTableIndexInfo::TPtr>;
    TUndoStack<TIndexState> Indexes;

    using TCdcStreamState = std::pair<TPathId, TCdcStreamInfo::TPtr>;
    TUndoStack<TCdcStreamState> CdcStreams;

    using TTableSnapshotState = std::pair<TPathId, TTxId>;
    TUndoStack<TTableSnapshotState> TablesWithSnapshots;

    using TLockState = std::pair<TPathId, TTxId>;
    TUndoStack<TLockState> LockedPaths;

    using TTableState = std::pair<TPathId, TTableInfo::TPtr>;
    TUndoStack<TTableState> Tables;

    using TColumnTableState = std::pair<TPathId, TColumnTableInfo::TPtr>;
    TUndoStack<TColumnTableState> ColumnTables;

    using TSequenceState = std::pair<TPathId, TSequenceInfo::TPtr>;
    TUndoStack<TSequenceState> Sequences;

    using TShardState = std::pair<TShardIdx, THolder<TShardInfo>>;
    TUndoStack<TShardState> Shards;

    // Actually, any single subdomain should not be grabbed at more than one version
    // per transaction/operation.
    // And transaction/operation could not work on more than one subdomain.
    // But just to be on the safe side (migrated paths, anyone?) we allow several
    // subdomains to be grabbed.
    THashMap<TPathId, TSubDomainInfo::TPtr> SubDomains;

    using TTxState = std::pair<TOperationId, THolder<TTxState>>;
    TUndoStack<TTxState> TxStates;

    using TExternalTableState = std::pair<TPathId, TExternalTableInfo::TPtr>;
    TUndoStack<TExternalTableState> ExternalTables;

    using TExternalDataSourceState = std::pair<TPathId, TExternalDataSourceInfo::TPtr>;
    TUndoStack<TExternalDataSourceState> ExternalDataSources;

    using TViewState = std::pair<TPathId, TViewInfo::TPtr>;
    TUndoStack<TViewState> Views;

    using TResourcePoolState = std::pair<TPathId, TResourcePoolInfo::TPtr>;
    TUndoStack<TResourcePoolState> ResourcePools;

    using TBackupCollectionState = std::pair<TPathId, TBackupCollectionInfo::TPtr>;
    TUndoStack<TBackupCollectionState> BackupCollections;

    using TSysViewState = std::pair<TPathId, TSysViewInfo::TPtr>;
    TUndoStack<TSysViewState> SysViews;

    using TLongIncrementalRestoreOpState = std::pair<TOperationId, std::optional<NKikimrSchemeOp::TLongIncrementalRestoreOp>>;
    TUndoStack<TLongIncrementalRestoreOpState> LongIncrementalRestoreOps;

    using TIncrementalBackupState = std::pair<ui64, TIncrementalBackupInfo::TPtr>;
    TUndoStack<TIncrementalBackupState> IncrementalBackups;

    // Mirrors IncrementalBackups: UnDo erases the id from Self->FullBackups.
    using TFullBackupState = std::pair<ui64, TFullBackupInfo::TPtr>;
    TUndoStack<TFullBackupState> FullBackups;

    // UnDo erases the (bcPathId -> id) entry, keeping BCPathToFullBackup atomic with FullBackups.
    using TBCPathToFullBackupState = std::pair<TPathId, std::optional<ui64>>;
    TUndoStack<TBCPathToFullBackupState> BCPathToFullBackup;

    using TSecretState = std::pair<TPathId, TSecretInfo::TPtr>;
    TUndoStack<TSecretState> Secrets;

    using TStreamingQueryState = std::pair<TPathId, TStreamingQueryInfo::TPtr>;
    TUndoStack<TStreamingQueryState> StreamingQueries;

    using TSharedShardEntry = std::tuple<TShardIdx, TPathId, std::optional<TTxId>>;
    TUndoStack<TSharedShardEntry> SharedShardEntries;

    using TTestShardSetState = std::pair<TPathId, TTestShardSetInfo::TPtr>;
    TUndoStack<TTestShardSetState> TestShardSets;

public:
    // Number of entries on every TPathId-keyed undo stack at some point in
    // time. Entries pushed after a mark are exactly the in-memory writes made
    // by the code that ran since it was taken.
    struct TMark {
        size_t Paths = 0;
        size_t Indexes = 0;
        size_t CdcStreams = 0;
        size_t TablesWithSnapshots = 0;
        size_t LockedPaths = 0;
        size_t Tables = 0;
        size_t ColumnTables = 0;
        size_t Sequences = 0;
        size_t ExternalTables = 0;
        size_t ExternalDataSources = 0;
        size_t Views = 0;
        size_t ResourcePools = 0;
        size_t BackupCollections = 0;
        size_t SysViews = 0;
        size_t BCPathToFullBackup = 0;
        size_t Secrets = 0;
        size_t StreamingQueries = 0;
        size_t SharedShardEntries = 0;
        size_t TestShardSets = 0;
    };

    TMark Mark() const;

    // Appends, in grab order and without repeating a path id already in `out`,
    // every TPathId grabbed after `mark`. Shards, TxStates,
    // LongIncrementalRestoreOps, IncrementalBackups and FullBackups are not
    // keyed by a path and are skipped; so is SubDomains, whose only entry is
    // the database path the caller already knows.
    void CollectPathIdsSince(const TMark& mark, TVector<TPathId>& out) const;

    ~TMemoryChanges() = default;

    void GrabNewTxState(TSchemeShard* ss, const TOperationId& op);

    void GrabNewPath(TSchemeShard* ss, const TPathId& pathId);
    void GrabPath(TSchemeShard* ss, const TPathId& pathId);

    void GrabNewTable(TSchemeShard* ss, const TPathId& pathId);
    void GrabTable(TSchemeShard* ss, const TPathId& pathId);

    void GrabNewColumnTable(TSchemeShard* ss, const TPathId& pathId);
    void GrabColumnTable(TSchemeShard* ss, const TPathId& pathId);

    void GrabNewShard(TSchemeShard* ss, const TShardIdx& shardId);
    void GrabShard(TSchemeShard* ss, const TShardIdx& shardId);

    void GrabDomain(TSchemeShard* ss, const TPathId& pathId);

    void GrabNewIndex(TSchemeShard* ss, const TPathId& pathId);
    void GrabIndex(TSchemeShard* ss, const TPathId& pathId);

    void GrabNewSequence(TSchemeShard* ss, const TPathId& pathId);
    void GrabSequence(TSchemeShard* ss, const TPathId& pathId);

    void GrabNewCdcStream(TSchemeShard* ss, const TPathId& pathId);
    void GrabCdcStream(TSchemeShard* ss, const TPathId& pathId);

    void GrabNewTableSnapshot(TSchemeShard* ss, const TPathId& pathId, TTxId snapshotTxId);

    void GrabNewLongLock(TSchemeShard* ss, const TPathId& pathId);
    void GrabLongLock(TSchemeShard* ss, const TPathId& pathId, TTxId lockTxId);

    void GrabNewExternalTable(TSchemeShard* ss, const TPathId& pathId);
    void GrabExternalTable(TSchemeShard* ss, const TPathId& pathId);

    void GrabNewExternalDataSource(TSchemeShard* ss, const TPathId& pathId);
    void GrabExternalDataSource(TSchemeShard* ss, const TPathId& pathId);

    void GrabNewView(TSchemeShard* ss, const TPathId& pathId);
    void GrabView(TSchemeShard* ss, const TPathId& pathId);

    void GrabNewResourcePool(TSchemeShard* ss, const TPathId& pathId);
    void GrabResourcePool(TSchemeShard* ss, const TPathId& pathId);

    void GrabNewBackupCollection(TSchemeShard* ss, const TPathId& pathId);
    void GrabBackupCollection(TSchemeShard* ss, const TPathId& pathId);

    void GrabNewSysView(TSchemeShard* ss, const TPathId& pathId);
    void GrabSysView(TSchemeShard* ss, const TPathId& pathId);

    void GrabNewLongIncrementalRestoreOp(TSchemeShard* ss, const TOperationId& opId);
    void GrabLongIncrementalRestoreOp(TSchemeShard* ss, const TOperationId& opId);

    void GrabNewLongIncrementalBackupOp(TSchemeShard* ss, ui64 id);

    void GrabNewFullBackupOp(TSchemeShard* ss, ui64 id);
    void GrabNewBCPathToFullBackup(TSchemeShard* ss, const TPathId& bcPathId);

    void GrabNewSecret(TSchemeShard* ss, const TPathId& pathId);
    void GrabSecret(TSchemeShard* ss, const TPathId& pathId);

    void GrabNewStreamingQuery(TSchemeShard* ss, const TPathId& pathId);
    void GrabStreamingQuery(TSchemeShard* ss, const TPathId& pathId);

    void GrabNewSharedShard(TSchemeShard* ss, const TShardIdx& shardIdx, const TPathId& pathId);
    void GrabSharedShard(TSchemeShard* ss, const TShardIdx& shardIdx, const TPathId& pathId);

    void GrabNewTestShardSet(TSchemeShard* ss, const TPathId& pathId);
    void GrabTestShardSet(TSchemeShard* ss, const TPathId& pathId);

    void UnDo(TSchemeShard* ss);
};

}
