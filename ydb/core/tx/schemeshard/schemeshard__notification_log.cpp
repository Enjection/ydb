#include "schemeshard_impl.h"

namespace NKikimr::NSchemeShard {

ui64 TSchemeShard::AllocateNotificationSequenceId(NIceDb::TNiceDb& db) {
    ui64 id = ++NextNotificationSequenceId;
    PersistUpdateNextNotificationSequenceId(db);
    return id;
}

void TSchemeShard::PersistNotificationLogEntry(
    NIceDb::TNiceDb& db,
    ui64 sequenceId,
    TTxId txId,
    TTxState::ETxType txType,
    TPathId pathId,
    const TString& pathName,
    NKikimrSchemeOp::EPathType objectType,
    NKikimrScheme::EStatus status,
    const TString& userSid,
    ui64 schemaVersion,
    const TString& description,
    TInstant completedAt)
{
    using T = Schema::NotificationLog;
    db.Table<T>().Key(sequenceId).Update(
        NIceDb::TUpdate<T::TxId>(ui64(txId)),
        NIceDb::TUpdate<T::OperationType>(ui32(txType)),
        NIceDb::TUpdate<T::PathOwnerId>(pathId.OwnerId),
        NIceDb::TUpdate<T::PathLocalId>(pathId.LocalPathId),
        NIceDb::TUpdate<T::PathName>(pathName),
        NIceDb::TUpdate<T::ObjectType>(ui32(objectType)),
        NIceDb::TUpdate<T::Status>(ui32(status)),
        NIceDb::TUpdate<T::UserSID>(userSid),
        NIceDb::TUpdate<T::SchemaVersion>(schemaVersion),
        NIceDb::TUpdate<T::ChangeDetails>(TString()),
        NIceDb::TUpdate<T::Description>(description),
        NIceDb::TUpdate<T::CompletedAt>(completedAt.MicroSeconds())
    );
    ++NotificationLogEntryCount;
    PersistUpdateNotificationLogEntryCount(db);
}

// Test-only: handle internal read of notification log
void TSchemeShard::Handle(TEvSchemeShard::TEvInternalReadNotificationLog::TPtr& ev, const TActorContext& ctx) {
    Execute(CreateTxInternalReadNotificationLog(ev), ctx);
}

// Transaction that reads notification log entries and returns them
struct TTxInternalReadNotificationLog : public NTabletFlatExecutor::TTransactionBase<TSchemeShard> {
    TEvSchemeShard::TEvInternalReadNotificationLog::TPtr Request;
    THolder<TEvSchemeShard::TEvInternalReadNotificationLogResult> Result;

    TTxInternalReadNotificationLog(TSchemeShard* self, TEvSchemeShard::TEvInternalReadNotificationLog::TPtr& ev)
        : TTransactionBase(self)
        , Request(ev)
        , Result(MakeHolder<TEvSchemeShard::TEvInternalReadNotificationLogResult>())
    {}

    bool Execute(TTransactionContext& txc, const TActorContext&) override {
        NIceDb::TNiceDb db(txc.DB);

        using NL = Schema::NotificationLog;
        auto rowset = db.Table<NL>().Range().Select();
        if (!rowset.IsReady()) {
            return false;
        }

        while (!rowset.EndOfSet()) {
            TEvSchemeShard::TEvInternalReadNotificationLogResult::TEntry entry;
            entry.SequenceId    = rowset.GetValue<NL::SequenceId>();
            entry.TxId          = rowset.GetValue<NL::TxId>();
            entry.OperationType = rowset.GetValue<NL::OperationType>();
            entry.PathOwnerId   = rowset.GetValue<NL::PathOwnerId>();
            entry.PathLocalId   = rowset.GetValue<NL::PathLocalId>();
            entry.PathName      = rowset.GetValue<NL::PathName>();
            entry.ObjectType    = rowset.GetValue<NL::ObjectType>();
            entry.Status        = rowset.GetValue<NL::Status>();
            entry.UserSID       = rowset.GetValue<NL::UserSID>();
            entry.SchemaVersion = rowset.GetValue<NL::SchemaVersion>();
            entry.Description   = rowset.GetValue<NL::Description>();
            entry.CompletedAt   = rowset.GetValue<NL::CompletedAt>();
            Result->Entries.push_back(std::move(entry));

            if (!rowset.Next()) {
                return false;
            }
        }

        return true;
    }

    void Complete(const TActorContext& ctx) override {
        ctx.Send(Request->Sender, Result.Release());
    }
};

NTabletFlatExecutor::ITransaction* TSchemeShard::CreateTxInternalReadNotificationLog(TEvSchemeShard::TEvInternalReadNotificationLog::TPtr& ev) {
    return new TTxInternalReadNotificationLog(this, ev);
}

} // namespace NKikimr::NSchemeShard
