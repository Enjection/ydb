#include "schemeshard_impl.h"

namespace NKikimr::NSchemeShard {

// TTxNotificationLogCleanup - periodic cleanup of old notification log entries
struct TTxNotificationLogCleanup : public NTabletFlatExecutor::TTransactionBase<TSchemeShard> {
    TTxNotificationLogCleanup(TSchemeShard* self)
        : TTransactionBase(self)
    {}

    bool Execute(TTransactionContext& txc, const TActorContext&) override {
        NIceDb::TNiceDb db(txc.DB);

        // Step 1: Read all subscriber cursors
        auto subRowset = db.Table<Schema::NotificationLogSubscriberCursors>().Range().Select();
        if (!subRowset.IsReady()) {
            return false;
        }

        ui64 minCursor = Max<ui64>(); // will find minimum across subscribers
        bool hasSubscribers = false;

        while (!subRowset.EndOfSet()) {
            ui64 cursor = subRowset.GetValue<Schema::NotificationLogSubscriberCursors::LastAckedSequenceId>();
            hasSubscribers = true;
            minCursor = Min(minCursor, cursor);

            if (!subRowset.Next()) {
                return false;
            }
        }

        if (!hasSubscribers) {
            minCursor = Self->NextNotificationSequenceId;
        }

        if (minCursor == 0 || minCursor == Max<ui64>()) {
            return true; // Nothing to clean
        }

        // Step 2: Delete entries up to minCursor
        ui64 deletedCount = 0;
        const ui64 maxDeletesPerBatch = 10000;

        auto logRowset = db.Table<Schema::NotificationLog>().Range().Select();
        if (!logRowset.IsReady()) {
            return false;
        }

        while (!logRowset.EndOfSet()) {
            ui64 seqId = logRowset.GetValue<Schema::NotificationLog::SequenceId>();
            if (seqId > minCursor) {
                break;
            }
            if (deletedCount >= maxDeletesPerBatch) {
                break; // Batch limit
            }

            db.Table<Schema::NotificationLog>().Key(seqId).Delete();
            ++deletedCount;

            if (!logRowset.Next()) {
                return false;
            }
        }

        if (deletedCount > 0 && Self->NotificationLogEntryCount >= deletedCount) {
            Self->NotificationLogEntryCount -= deletedCount;
            Self->PersistUpdateNotificationLogEntryCount(db);
        }

        return true;
    }

    void Complete(const TActorContext& ctx) override {
        // Schedule next cleanup run
        Self->ScheduleNotificationLogCleanup(ctx);
    }
};

// TTxForceAdvanceSubscriber - admin command to force-advance a subscriber's cursor
struct TTxForceAdvanceSubscriber : public NTabletFlatExecutor::TTransactionBase<TSchemeShard> {
    TEvSchemeShard::TEvForceAdvanceSubscriber::TPtr Request;
    THolder<TEvSchemeShard::TEvForceAdvanceSubscriberResult> Result;

    TTxForceAdvanceSubscriber(TSchemeShard* self, TEvSchemeShard::TEvForceAdvanceSubscriber::TPtr& ev)
        : TTransactionBase(self)
        , Request(ev)
        , Result(MakeHolder<TEvSchemeShard::TEvForceAdvanceSubscriberResult>())
    {}

    bool Execute(TTransactionContext& txc, const TActorContext&) override {
        const auto& record = Request->Get()->Record;
        TString subscriberId = record.GetSubscriberId();

        NIceDb::TNiceDb db(txc.DB);

        // Verify subscriber exists
        auto rowset = db.Table<Schema::NotificationLogSubscriberCursors>().Key(subscriberId).Select();
        if (!rowset.IsReady()) {
            return false;
        }

        if (!rowset.IsValid()) {
            Result->Record.SetStatus(NKikimrScheme::StatusPathDoesNotExist);
            Result->Record.SetReason("Subscriber not registered: " + subscriberId);
            return true;
        }

        // Advance cursor to current tail (NextNotificationSequenceId is the last assigned ID)
        ui64 newCursor = Self->NextNotificationSequenceId;

        db.Table<Schema::NotificationLogSubscriberCursors>().Key(subscriberId).Update(
            NIceDb::TUpdate<Schema::NotificationLogSubscriberCursors::LastAckedSequenceId>(newCursor),
            NIceDb::TUpdate<Schema::NotificationLogSubscriberCursors::LastActivityAt>(TInstant::Now().MicroSeconds())
        );

        Result->Record.SetStatus(NKikimrScheme::StatusSuccess);
        Result->Record.SetNewCursor(newCursor);

        return true;
    }

    void Complete(const TActorContext& ctx) override {
        ctx.Send(Request->Sender, Result.Release());
    }
};

// Factory methods and handlers
NTabletFlatExecutor::ITransaction* TSchemeShard::CreateTxNotificationLogCleanup() {
    return new TTxNotificationLogCleanup(this);
}

NTabletFlatExecutor::ITransaction* TSchemeShard::CreateTxForceAdvanceSubscriber(TEvSchemeShard::TEvForceAdvanceSubscriber::TPtr& ev) {
    return new TTxForceAdvanceSubscriber(this, ev);
}

void TSchemeShard::Handle(TEvSchemeShard::TEvForceAdvanceSubscriber::TPtr& ev, const TActorContext& ctx) {
    Execute(CreateTxForceAdvanceSubscriber(ev), ctx);
}

void TSchemeShard::Handle(TEvSchemeShard::TEvWakeupToRunNotificationLogCleanup::TPtr&, const TActorContext& ctx) {
    HandleWakeupToRunNotificationLogCleanup(ctx);
}

void TSchemeShard::HandleWakeupToRunNotificationLogCleanup(const TActorContext& ctx) {
    Execute(CreateTxNotificationLogCleanup(), ctx);
}

void TSchemeShard::ScheduleNotificationLogCleanup(const TActorContext& ctx) {
    ctx.Schedule(TDuration::Hours(1), new TEvSchemeShard::TEvWakeupToRunNotificationLogCleanup());
}

} // namespace NKikimr::NSchemeShard
