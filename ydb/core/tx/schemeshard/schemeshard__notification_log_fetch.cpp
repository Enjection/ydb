#include "schemeshard_impl.h"

namespace NKikimr::NSchemeShard {

// ==================== TTxRegisterSubscriber ====================

struct TTxRegisterSubscriber : public NTabletFlatExecutor::TTransactionBase<TSchemeShard> {
    TEvSchemeShard::TEvRegisterSubscriber::TPtr Request;
    THolder<TEvSchemeShard::TEvRegisterSubscriberResult> Result;

    TTxRegisterSubscriber(TSchemeShard* self, TEvSchemeShard::TEvRegisterSubscriber::TPtr& ev)
        : TTransactionBase(self)
        , Request(ev)
        , Result(MakeHolder<TEvSchemeShard::TEvRegisterSubscriberResult>())
    {}

    bool Execute(TTransactionContext& txc, const TActorContext&) override {
        const auto& record = Request->Get()->Record;
        TString subscriberId = record.GetSubscriberId();

        NIceDb::TNiceDb db(txc.DB);

        // Check if subscriber already exists
        auto rowset = db.Table<Schema::SubscriberCursors>().Key(subscriberId).Select();
        if (!rowset.IsReady()) {
            return false;
        }

        if (rowset.IsValid()) {
            // Already registered - return current cursor
            ui64 currentCursor = rowset.GetValue<Schema::SubscriberCursors::LastAckedSequenceId>();
            Result->Record.SetStatus(NKikimrScheme::StatusSuccess);
            Result->Record.SetCurrentSequenceId(currentCursor);
        } else {
            // New subscriber - create with cursor at 0
            db.Table<Schema::SubscriberCursors>().Key(subscriberId).Update(
                NIceDb::TUpdate<Schema::SubscriberCursors::LastAckedSequenceId>(0),
                NIceDb::TUpdate<Schema::SubscriberCursors::LastActivityAt>(TInstant::Now().MicroSeconds())
            );
            Result->Record.SetStatus(NKikimrScheme::StatusSuccess);
            Result->Record.SetCurrentSequenceId(0);
        }

        return true;
    }

    void Complete(const TActorContext& ctx) override {
        ctx.Send(Request->Sender, Result.Release());
    }
};

// ==================== TTxFetchNotifications ====================

struct TTxFetchNotifications : public NTabletFlatExecutor::TTransactionBase<TSchemeShard> {
    TEvSchemeShard::TEvFetchNotifications::TPtr Request;
    THolder<TEvSchemeShard::TEvFetchNotificationsResult> Result;

    TTxFetchNotifications(TSchemeShard* self, TEvSchemeShard::TEvFetchNotifications::TPtr& ev)
        : TTransactionBase(self)
        , Request(ev)
        , Result(MakeHolder<TEvSchemeShard::TEvFetchNotificationsResult>())
    {}

    bool Execute(TTransactionContext& txc, const TActorContext&) override {
        const auto& record = Request->Get()->Record;
        TString subscriberId = record.GetSubscriberId();
        ui64 afterSeqId = record.GetAfterSequenceId();
        ui32 maxCount = record.GetMaxCount();

        if (maxCount == 0 || maxCount > 1000) {
            maxCount = 1000;
        }

        NIceDb::TNiceDb db(txc.DB);

        // Verify subscriber exists
        auto subRowset = db.Table<Schema::SubscriberCursors>().Key(subscriberId).Select();
        if (!subRowset.IsReady()) {
            return false;
        }

        if (!subRowset.IsValid()) {
            Result->Record.SetStatus(NKikimrScheme::StatusPathDoesNotExist);
            Result->Record.SetReason("Subscriber not registered: " + subscriberId);
            return true;
        }

        ui64 storedCursor = subRowset.GetValue<Schema::SubscriberCursors::LastAckedSequenceId>();

        // Calculate skipped entries if afterSeqId < storedCursor
        // (subscriber's stored cursor was force-advanced past their request)
        ui64 effectiveAfterSeqId = afterSeqId;
        ui64 skippedEntries = 0;
        if (storedCursor > afterSeqId) {
            skippedEntries = storedCursor - afterSeqId;
            effectiveAfterSeqId = storedCursor;
        }

        // Read entries from NotificationLog starting after effectiveAfterSeqId
        Y_ENSURE(effectiveAfterSeqId < Max<ui64>(), "effectiveAfterSeqId overflow");
        auto rowset = db.Table<Schema::NotificationLog>().GreaterOrEqual(effectiveAfterSeqId + 1).Select();
        if (!rowset.IsReady()) {
            return false;
        }

        ui32 count = 0;
        ui64 lastSeqId = effectiveAfterSeqId;
        bool hasMore = false;

        while (!rowset.EndOfSet()) {
            if (count >= maxCount) {
                hasMore = true;
                break;
            }

            auto* entry = Result->Record.AddEntries();
            ui64 seqId = rowset.GetValue<Schema::NotificationLog::SequenceId>();
            entry->SetSequenceId(seqId);
            entry->SetTxId(rowset.GetValue<Schema::NotificationLog::TxId>());
            entry->SetOperationType(rowset.GetValue<Schema::NotificationLog::OperationType>());
            entry->SetPathOwnerId(rowset.GetValue<Schema::NotificationLog::PathOwnerId>());
            entry->SetPathLocalId(rowset.GetValue<Schema::NotificationLog::PathLocalId>());
            entry->SetPathName(rowset.GetValue<Schema::NotificationLog::PathName>());
            entry->SetObjectType(rowset.GetValue<Schema::NotificationLog::ObjectType>());
            entry->SetStatus(rowset.GetValue<Schema::NotificationLog::Status>());
            entry->SetUserSID(rowset.GetValue<Schema::NotificationLog::UserSID>());
            entry->SetSchemaVersion(rowset.GetValue<Schema::NotificationLog::SchemaVersion>());
            entry->SetDescription(rowset.GetValue<Schema::NotificationLog::Description>());
            entry->SetCompletedAt(rowset.GetValue<Schema::NotificationLog::CompletedAt>());
            entry->SetPlanStep(rowset.GetValueOrDefault<Schema::NotificationLog::PlanStep>(0));

            lastSeqId = seqId;
            ++count;

            if (!rowset.Next()) {
                return false;
            }
        }

        // Update LastActivityAt on every fetch (prevents subscriber from appearing stale)
        db.Table<Schema::SubscriberCursors>().Key(subscriberId).Update(
            NIceDb::TUpdate<Schema::SubscriberCursors::LastActivityAt>(TInstant::Now().MicroSeconds())
        );

        Result->Record.SetStatus(NKikimrScheme::StatusSuccess);
        Result->Record.SetLastSequenceId(lastSeqId);
        Result->Record.SetHasMore(hasMore);
        Result->Record.SetSkippedEntries(skippedEntries);

        // Compute MinInFlightPlanStep watermark
        ui64 minInFlightPlanStep = 0;
        for (const auto& [opId, txState] : Self->TxInFlight) {
            if (txState.PlanStep != InvalidStepId
                && txState.State != TTxState::Done
                && txState.State != TTxState::Aborted) {
                ui64 ps = ui64(txState.PlanStep.GetValue());
                if (minInFlightPlanStep == 0 || ps < minInFlightPlanStep) {
                    minInFlightPlanStep = ps;
                }
            }
        }
        Result->Record.SetMinInFlightPlanStep(minInFlightPlanStep);

        return true;
    }

    void Complete(const TActorContext& ctx) override {
        ctx.Send(Request->Sender, Result.Release());
    }
};

// ==================== TTxAckNotifications ====================

struct TTxAckNotifications : public NTabletFlatExecutor::TTransactionBase<TSchemeShard> {
    TEvSchemeShard::TEvAckNotifications::TPtr Request;
    THolder<TEvSchemeShard::TEvAckNotificationsResult> Result;

    TTxAckNotifications(TSchemeShard* self, TEvSchemeShard::TEvAckNotifications::TPtr& ev)
        : TTransactionBase(self)
        , Request(ev)
        , Result(MakeHolder<TEvSchemeShard::TEvAckNotificationsResult>())
    {}

    bool Execute(TTransactionContext& txc, const TActorContext&) override {
        const auto& record = Request->Get()->Record;
        TString subscriberId = record.GetSubscriberId();
        ui64 upToSeqId = record.GetUpToSequenceId();

        NIceDb::TNiceDb db(txc.DB);

        // Verify subscriber exists
        auto rowset = db.Table<Schema::SubscriberCursors>().Key(subscriberId).Select();
        if (!rowset.IsReady()) {
            return false;
        }

        if (!rowset.IsValid()) {
            Result->Record.SetStatus(NKikimrScheme::StatusPathDoesNotExist);
            Result->Record.SetReason("Subscriber not registered: " + subscriberId);
            return true;
        }

        ui64 currentCursor = rowset.GetValue<Schema::SubscriberCursors::LastAckedSequenceId>();

        // Only advance cursor forward, never backward
        ui64 newCursor = Max(currentCursor, upToSeqId);

        // Clamp to the current max sequence ID (NextNotificationSequenceId is the last assigned ID)
        if (newCursor > Self->NextNotificationSequenceId) {
            newCursor = Self->NextNotificationSequenceId;
        }

        db.Table<Schema::SubscriberCursors>().Key(subscriberId).Update(
            NIceDb::TUpdate<Schema::SubscriberCursors::LastAckedSequenceId>(newCursor),
            NIceDb::TUpdate<Schema::SubscriberCursors::LastActivityAt>(TInstant::Now().MicroSeconds())
        );

        Result->Record.SetStatus(NKikimrScheme::StatusSuccess);
        Result->Record.SetNewCursor(newCursor);

        return true;
    }

    void Complete(const TActorContext& ctx) override {
        ctx.Send(Request->Sender, Result.Release());
    }
};

// ==================== Factory methods ====================

NTabletFlatExecutor::ITransaction* TSchemeShard::CreateTxRegisterSubscriber(TEvSchemeShard::TEvRegisterSubscriber::TPtr& ev) {
    return new TTxRegisterSubscriber(this, ev);
}

NTabletFlatExecutor::ITransaction* TSchemeShard::CreateTxFetchNotifications(TEvSchemeShard::TEvFetchNotifications::TPtr& ev) {
    return new TTxFetchNotifications(this, ev);
}

NTabletFlatExecutor::ITransaction* TSchemeShard::CreateTxAckNotifications(TEvSchemeShard::TEvAckNotifications::TPtr& ev) {
    return new TTxAckNotifications(this, ev);
}

// ==================== Handle methods ====================

void TSchemeShard::Handle(TEvSchemeShard::TEvRegisterSubscriber::TPtr& ev, const TActorContext& ctx) {
    Execute(CreateTxRegisterSubscriber(ev), ctx);
}

void TSchemeShard::Handle(TEvSchemeShard::TEvFetchNotifications::TPtr& ev, const TActorContext& ctx) {
    Execute(CreateTxFetchNotifications(ev), ctx);
}

void TSchemeShard::Handle(TEvSchemeShard::TEvAckNotifications::TPtr& ev, const TActorContext& ctx) {
    Execute(CreateTxAckNotifications(ev), ctx);
}

} // namespace NKikimr::NSchemeShard
