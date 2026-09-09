#include "schemeshard_impl.h"

namespace NKikimr::NSchemeShard {

TMaybe<TNativeOperationReplay> TSchemeShard::FindNativeOperationByUid(const TNativeOperationKey& key) const {
    const auto* id = NativeOperationsByUid.FindPtr(key);
    if (!id) {
        return Nothing();
    }
    switch (key.first) {
        case NKikimrSchemeOp::ESchemeOpBackupBackupCollection: {
            const auto& info = *FullBackups.at(*id);
            return TNativeOperationReplay{*id, info.DomainPathId, info.OriginalDdl, info.UserSID.GetOrElse(TString())};
        }
        case NKikimrSchemeOp::ESchemeOpBackupIncrementalBackupCollection: {
            const auto& info = *IncrementalBackups.at(*id);
            return TNativeOperationReplay{*id, info.DomainPathId, info.OriginalDdl, info.UserSID.GetOrElse(TString())};
        }
        case NKikimrSchemeOp::ESchemeOpRestoreBackupCollection: {
            const auto& info = IncrementalRestoreStates.at(*id);
            return TNativeOperationReplay{*id, info.DomainPathId, info.OriginalDdl, info.UserSID};
        }
        default:
            Y_ABORT("Unexpected native UID operation family");
    }
}

void TSchemeShard::BindNativeOperationUid(const TNativeOperationKey& key, ui64 id,
    const NKikimrSchemeOp::TModifyScheme& tx, const TPathId& domainPathId, const TString& userSID)
{
    const auto& ddl = tx.GetNativeOperationIdentity().GetOriginalDdl();
    switch (key.first) {
        case NKikimrSchemeOp::ESchemeOpBackupBackupCollection: {
            auto& info = *FullBackups.at(id);
            info.Uid = key.second;
            info.OriginalDdl = ddl;
            info.UserSID = userSID;
            break;
        }
        case NKikimrSchemeOp::ESchemeOpBackupIncrementalBackupCollection: {
            auto& info = *IncrementalBackups.at(id);
            info.Uid = key.second;
            info.OriginalDdl = ddl;
            info.UserSID = userSID;
            break;
        }
        case NKikimrSchemeOp::ESchemeOpRestoreBackupCollection: {
            Y_ABORT_UNLESS(!IncrementalRestoreStates.contains(id));
            auto& info = IncrementalRestoreStates[id];
            info.Uid = key.second;
            info.OriginalDdl = ddl;
            info.DomainPathId = domainPathId;
            info.UserSID = userSID;
            info.OriginalOperationId = id;
            const auto& name = tx.GetRestoreBackupCollection().GetName();
            const auto path = TPath::Resolve(name.StartsWith('/') ? name : tx.GetWorkingDir() + "/" + name, this);
            Y_ABORT_UNLESS(path.IsResolved());
            info.BackupCollectionPathId = path.Base()->PathId;
            info.AwaitingInitialRestore = true;
            break;
        }
        default:
            Y_ABORT("Unexpected native UID operation family");
    }
    Y_ABORT_UNLESS(NativeOperationsByUid.emplace(key, id).second);
}

void TSchemeShard::PersistNativeOperationKey(NIceDb::TNiceDb& db, const TNativeOperationKey& key) {
    const auto id = NativeOperationsByUid.at(key);
    switch (key.first) {
        case NKikimrSchemeOp::ESchemeOpBackupBackupCollection: {
            const auto& info = *FullBackups.at(id);
            db.Table<Schema::FullBackups>().Key(id).Update(
                NIceDb::TUpdate<Schema::FullBackups::Uid>(info.Uid),
                NIceDb::TUpdate<Schema::FullBackups::OriginalDdl>(info.OriginalDdl));
            break;
        }
        case NKikimrSchemeOp::ESchemeOpBackupIncrementalBackupCollection: {
            const auto& info = *IncrementalBackups.at(id);
            db.Table<Schema::IncrementalBackups>().Key(id).Update(
                NIceDb::TUpdate<Schema::IncrementalBackups::Uid>(info.Uid),
                NIceDb::TUpdate<Schema::IncrementalBackups::OriginalDdl>(info.OriginalDdl));
            break;
        }
        case NKikimrSchemeOp::ESchemeOpRestoreBackupCollection: {
            const auto& info = IncrementalRestoreStates.at(id);
            using T = Schema::IncrementalRestoreState;
            db.Table<T>().Key(id).Update(
                NIceDb::TUpdate<T::Uid>(info.Uid),
                NIceDb::TUpdate<T::OriginalDdl>(info.OriginalDdl),
                NIceDb::TUpdate<T::DomainPathOwnerId>(info.DomainPathId.OwnerId),
                NIceDb::TUpdate<T::DomainPathId>(info.DomainPathId.LocalPathId),
                NIceDb::TUpdate<T::UserSID>(info.UserSID),
                NIceDb::TUpdate<T::BackupCollectionPathOwnerId>(info.BackupCollectionPathId.OwnerId),
                NIceDb::TUpdate<T::BackupCollectionPathId>(info.BackupCollectionPathId.LocalPathId),
                NIceDb::TUpdate<T::State>(static_cast<ui32>(info.State)),
                NIceDb::TUpdate<T::CurrentIncrementalIdx>(info.CurrentIncrementalIdx),
                NIceDb::TUpdate<T::AwaitingInitialRestore>(info.AwaitingInitialRestore));
            break;
        }
        default:
            Y_ABORT("Unexpected native UID operation family");
    }
}

} // namespace NKikimr::NSchemeShard
