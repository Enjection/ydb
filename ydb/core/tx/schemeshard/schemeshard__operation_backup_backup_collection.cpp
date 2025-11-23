#include "schemeshard__backup_collection_common.h"
#include "schemeshard__op_traits.h"
#include "schemeshard__operation_common.h"
#include "schemeshard__operation_create_cdc_stream.h"
#include "schemeshard__operation_part.h"
#include "schemeshard_utils.h"
#include "schemeshard_impl.h"


namespace NKikimr::NSchemeShard {

using TTag = TSchemeTxTraits<NKikimrSchemeOp::EOperationType::ESchemeOpBackupBackupCollection>;

namespace NOperation {

template <>
std::optional<THashMap<TString, THashSet<TString>>> GetRequiredPaths<TTag>(
    TTag,
    const TTxTransaction& tx,
    const TOperationContext& context)
{
    const auto& backupOp = tx.GetBackupBackupCollection();
    return NBackup::GetBackupRequiredPaths(tx, backupOp.GetTargetDir(), backupOp.GetName(), context);
}

template <>
bool Rewrite(TTag, TTxTransaction& tx) {
    auto now = NBackup::ToX509String(TlsActivationContext->AsActorContext().Now());
    tx.MutableBackupBackupCollection()->SetTargetDir(now + "_full");
    return true;
}

} // namespace NOperation

TVector<ISubOperation::TPtr> CreateBackupBackupCollection(TOperationId opId, const TTxTransaction& tx, TOperationContext& context) {
    TVector<ISubOperation::TPtr> result;

    NKikimrSchemeOp::TModifyScheme modifyScheme;
    modifyScheme.SetOperationType(NKikimrSchemeOp::ESchemeOpCreateConsistentCopyTables);
    modifyScheme.SetInternal(true);

    auto& cct = *modifyScheme.MutableCreateConsistentCopyTables();
    auto& copyTables = *cct.MutableCopyTableDescriptions();
    const auto workingDirPath = TPath::Resolve(tx.GetWorkingDir(), context.SS);

    TString bcPathStr = JoinPath({tx.GetWorkingDir(), tx.GetBackupBackupCollection().GetName()});

    const TPath& bcPath = TPath::Resolve(bcPathStr, context.SS);
    {
        auto checks = bcPath.Check();
        checks
            .NotEmpty()
            .NotUnderDomainUpgrade()
            .IsAtLocalSchemeShard()
            .IsResolved()
            .NotUnderDeleting()
            .NotUnderOperation()
            .IsBackupCollection();

        if (!checks) {
            result = {CreateReject(opId, checks.GetStatus(), checks.GetError())};
            return result;
        }
    }

    Y_ABORT_UNLESS(context.SS->BackupCollections.contains(bcPath->PathId));
    const auto& bc = context.SS->BackupCollections[bcPath->PathId];
    bool incrBackupEnabled = bc->Description.HasIncrementalBackupConfig();
    
    bool omitIndexes = bc->Description.GetOmitIndexes() || 
                       (incrBackupEnabled && bc->Description.GetIncrementalBackupConfig().GetOmitIndexes());
    
    TString streamName = NBackup::ToX509String(TlsActivationContext->AsActorContext().Now()) + "_continuousBackupImpl";

    for (const auto& item : bc->Description.GetExplicitEntryList().GetEntries()) {
        auto& desc = *copyTables.Add();
        desc.SetSrcPath(item.GetPath());
        std::pair<TString, TString> paths;
        TString err;
        if (!TrySplitPathByDb(item.GetPath(), bcPath.GetDomainPathString(), paths, err)) {
            result = {CreateReject(opId, NKikimrScheme::StatusInvalidParameter, err)};
            return {};
        }
        auto& relativeItemPath = paths.second;
        desc.SetDstPath(JoinPath({tx.GetWorkingDir(), tx.GetBackupBackupCollection().GetName(), tx.GetBackupBackupCollection().GetTargetDir(), relativeItemPath}));
        
        // For incremental backups, omit indexes from main table copy since they're handled separately
        // with CDC stream info by CreateConsistentCopyTables
        // Don't force omit of indexes in the descriptor here —
        // CopyTableTask already sets CreateTable::OmitIndexes to tell
        // CreateCopyTable to skip its internal index recursion.
        // The descriptor's OmitIndexes controls whether
        // CreateConsistentCopyTables should process indexes; keep
        // that decision driven by the collection config (omitIndexes).
        desc.SetOmitIndexes(omitIndexes);
        
        desc.SetOmitFollowers(true);
        desc.SetAllowUnderSameOperation(true);

        // For incremental backups, create CDC stream on the source table
        if (incrBackupEnabled) {
            NKikimrSchemeOp::TCreateCdcStream createCdcStreamOp;
            // TableName should be just the table name, not the full path
            // The working directory will be set to the parent path
            const auto sPath = TPath::Resolve(item.GetPath(), context.SS);
            
            {
                auto checks = sPath.Check();
                checks
                    .IsResolved()
                    .NotDeleted()
                    .IsTable();
                
                if (!checks) {
                    result = {CreateReject(opId, checks.GetStatus(), checks.GetError())};
                    return result;
                }
            }
            
            createCdcStreamOp.SetTableName(sPath.LeafName());
            auto& streamDescription = *createCdcStreamOp.MutableStreamDescription();
            streamDescription.SetName(streamName);
            streamDescription.SetMode(NKikimrSchemeOp::ECdcStreamModeUpdate);
            streamDescription.SetFormat(NKikimrSchemeOp::ECdcStreamFormatProto);
            
            // Create CDC StreamImpl for the main table (metadata only, before copying starts)
            // The copy-table operation will use CreateCdcStream to link to this stream
            NCdc::DoCreateStreamImpl(result, createCdcStreamOp, opId, sPath, false, false);
            
            // Store CDC stream config in the descriptor - copy-table will create AtTable and PQ parts
            desc.MutableCreateSrcCdcStream()->CopyFrom(createCdcStreamOp);
            
            // Create CDC StreamImpl for index implementation tables (before copying starts)
            // Store CDC info in the descriptor so CreateConsistentCopyTables can create AtTable and PQ parts
            // Only do this for incremental backups
            if (incrBackupEnabled && !omitIndexes) {
                const auto tablePath = sPath;
                
                // Iterate through table's children to find indexes
                for (const auto& [childName, childPathId] : tablePath.Base()->GetChildren()) {
                    auto childPath = context.SS->PathsById.at(childPathId);
                    
                    // Skip non-index children (CDC streams, etc.)
                    if (childPath->PathType != NKikimrSchemeOp::EPathTypeTableIndex) {
                        continue;
                    }
                    
                    // Skip deleted indexes
                    if (childPath->Dropped()) {
                        continue;
                    }
                    
                    // Get index info and filter for global sync only
                    auto indexInfo = context.SS->Indexes.at(childPathId);
                    if (indexInfo->Type != NKikimrSchemeOp::EIndexTypeGlobal) {
                        continue;
                    }
                
                    // Get index implementation table (the only child of index)
                    auto indexPath = TPath::Init(childPathId, context.SS);
                    Y_ABORT_UNLESS(indexPath.Base()->GetChildren().size() == 1);
                    auto [implTableName, implTablePathId] = *indexPath.Base()->GetChildren().begin();
                    
                    auto indexTablePath = indexPath.Child(implTableName);
                    
                    // Create CDC stream on index impl table (before copying starts)
                    NKikimrSchemeOp::TCreateCdcStream indexCdcStreamOp;
                    // Set table name to just the name since we're passing the full path as tablePath parameter
                    indexCdcStreamOp.SetTableName(implTableName);
                    auto& indexStreamDescription = *indexCdcStreamOp.MutableStreamDescription();
                    indexStreamDescription.SetName(streamName);
                    indexStreamDescription.SetMode(NKikimrSchemeOp::ECdcStreamModeUpdate);
                    indexStreamDescription.SetFormat(NKikimrSchemeOp::ECdcStreamFormatProto);
                    
                    NCdc::DoCreateStreamImpl(result, indexCdcStreamOp, opId, indexTablePath, false, false);
                    
                    // Store CDC stream info in the descriptor's map
                    // Key is the impl table name, value is the CDC stream config
                    (*desc.MutableIndexImplTableCdcStreams())[implTableName].CopyFrom(indexCdcStreamOp);
                }
            }
            
            if (incrBackupEnabled && !omitIndexes) {
                // Also invalidate cache for index impl tables
                for (const auto& [childName, childPathId] : sPath.Base()->GetChildren()) {
                    auto childPath = context.SS->PathsById.at(childPathId);
                    if (childPath->PathType != NKikimrSchemeOp::EPathTypeTableIndex && !childPath->Dropped()) {
                        auto indexInfo = context.SS->Indexes.find(childPathId);
                        if (indexInfo != context.SS->Indexes.end() && 
                            indexInfo->second->Type == NKikimrSchemeOp::EIndexTypeGlobal) {
                            
                            auto indexPath = TPath::Init(childPathId, context.SS);
                            for (const auto& [implTableName, implTablePathId] : indexPath.Base()->GetChildren()) {
                                auto implTablePath = context.SS->PathsById.at(implTablePathId);
                                if (implTablePath->IsTable()) {
                                    context.SS->ClearDescribePathCaches(implTablePath);
                                    context.OnComplete.PublishToSchemeBoard(opId, implTablePathId);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    if (!CreateConsistentCopyTables(opId, modifyScheme, context, result)) {
        return result;
    }

    // Log version information after CDC stream creation for diagnostics
    if (incrBackupEnabled && !omitIndexes) {
        for (const auto& item : bc->Description.GetExplicitEntryList().GetEntries()) {
            const auto tablePath = TPath::Resolve(item.GetPath(), context.SS);
            if (!tablePath.IsResolved()) {
                continue;
            }
            
            auto table = context.SS->Tables.at(tablePath.Base()->PathId);
            
            LOG_NOTICE_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                "Backup CDC creation completed for table: " << tablePath.PathString()
                << ", MainTable AlterVersion: " << table->AlterVersion
                << ", PathId: " << tablePath.Base()->PathId);
            
            // Log index and index impl table versions
            for (const auto& [childName, childPathId] : tablePath.Base()->GetChildren()) {
                auto childPath = context.SS->PathsById.at(childPathId);
                
                if (childPath->PathType != NKikimrSchemeOp::EPathTypeTableIndex) {
                    continue;
                }
                
                if (childPath->Dropped()) {
                    continue;
                }
                
                auto indexInfo = context.SS->Indexes.at(childPathId);
                if (indexInfo->Type != NKikimrSchemeOp::EIndexTypeGlobal) {
                    continue;
                }
                
                LOG_NOTICE_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                    "Index: " << childName 
                    << ", Index AlterVersion: " << indexInfo->AlterVersion
                    << ", Index PathId: " << childPathId);
                
                auto indexPath = TPath::Init(childPathId, context.SS);
                Y_ABORT_UNLESS(indexPath.Base()->GetChildren().size() == 1);
                auto [implTableName, implTablePathId] = *indexPath.Base()->GetChildren().begin();
                
                auto implTable = context.SS->Tables.at(implTablePathId);
                LOG_NOTICE_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                    "IndexImplTable: " << implTableName
                    << ", AlterVersion: " << implTable->AlterVersion
                    << ", PathId: " << implTablePathId);
            }
        }
    }

    if (incrBackupEnabled) {
        for (const auto& item : bc->Description.GetExplicitEntryList().GetEntries()) {
            NKikimrSchemeOp::TCreateCdcStream createCdcStreamOp;
            createCdcStreamOp.SetTableName(item.GetPath());
            auto& streamDescription = *createCdcStreamOp.MutableStreamDescription();
            streamDescription.SetName(streamName);
            streamDescription.SetMode(NKikimrSchemeOp::ECdcStreamModeUpdate);
            streamDescription.SetFormat(NKikimrSchemeOp::ECdcStreamFormatProto);

            const auto sPath = TPath::Resolve(item.GetPath(), context.SS);
            
            {
                auto checks = sPath.Check();
                checks
                    .IsResolved()
                    .NotDeleted()
                    .IsTable();
                
                if (!checks) {
                    result = {CreateReject(opId, checks.GetStatus(), checks.GetError())};
                    return result;
                }
            }
            
            auto table = context.SS->Tables.at(sPath.Base()->PathId);

            TVector<TString> boundaries;
            const auto& partitions = table->GetPartitions();
            boundaries.reserve(partitions.size() - 1);

            for (ui32 i = 0; i < partitions.size(); ++i) {
                const auto& partition = partitions.at(i);
                if (i != partitions.size() - 1) {
                    boundaries.push_back(partition.EndOfRange);
                }
            }

            const auto streamPath = sPath.Child(streamName);

            NCdc::DoCreatePqPart(result, createCdcStreamOp, opId, streamPath, streamName, table, boundaries, false);
        }
        
        // Create PQ parts for index impl table CDC streams (after copying completes)
        // Only for incremental backups
        if (incrBackupEnabled && !omitIndexes) {
            for (const auto& item : bc->Description.GetExplicitEntryList().GetEntries()) {
                const auto tablePath = TPath::Resolve(item.GetPath(), context.SS);
                
                // Iterate through table's children to find indexes
                for (const auto& [childName, childPathId] : tablePath.Base()->GetChildren()) {
                    auto childPath = context.SS->PathsById.at(childPathId);
                    
                    // Skip non-index children
                    if (childPath->PathType != NKikimrSchemeOp::EPathTypeTableIndex) {
                        continue;
                    }
                    
                    // Skip deleted indexes
                    if (childPath->Dropped()) {
                        continue;
                    }
                    
                    // Get index info and filter for global sync only
                    auto indexInfo = context.SS->Indexes.at(childPathId);
                    if (indexInfo->Type != NKikimrSchemeOp::EIndexTypeGlobal) {
                        continue;
                    }
                
                    // Get index implementation table
                    auto indexPath = TPath::Init(childPathId, context.SS);
                    Y_ABORT_UNLESS(indexPath.Base()->GetChildren().size() == 1);
                    auto [implTableName, implTablePathId] = *indexPath.Base()->GetChildren().begin();
                    
                    auto indexTablePath = indexPath.Child(implTableName);
                    auto indexTable = context.SS->Tables.at(implTablePathId);
                    
                    // Create CDC stream metadata for PQ part
                    NKikimrSchemeOp::TCreateCdcStream indexCdcStreamOp;
                    indexCdcStreamOp.SetTableName(implTableName);
                    auto& indexStreamDescription = *indexCdcStreamOp.MutableStreamDescription();
                    indexStreamDescription.SetName(streamName);
                    indexStreamDescription.SetMode(NKikimrSchemeOp::ECdcStreamModeUpdate);
                    indexStreamDescription.SetFormat(NKikimrSchemeOp::ECdcStreamFormatProto);
                    
                    // Create PQ part for index CDC stream
                    TVector<TString> indexBoundaries;
                    const auto& indexPartitions = indexTable->GetPartitions();
                    indexBoundaries.reserve(indexPartitions.size() - 1);
                    for (ui32 i = 0; i < indexPartitions.size(); ++i) {
                        const auto& partition = indexPartitions.at(i);
                        if (i != indexPartitions.size() - 1) {
                            indexBoundaries.push_back(partition.EndOfRange);
                        }
                    }
                    
                    const auto indexStreamPath = indexTablePath.Child(streamName);
                    NCdc::DoCreatePqPart(result, indexCdcStreamOp, opId, indexStreamPath, streamName, indexTable, indexBoundaries, false);
                }
            }
        }
    }

    return result;
}

} // namespace NKikimr::NSchemeShard
