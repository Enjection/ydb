# SchemeShared Operation Handling - Technical Report

## Table of Contents
1. [Overview](#overview)
2. [Self-Continuation Pattern ("Meta" Operations)](#self-continuation-pattern)
3. [Operation Stages](#operation-stages)
4. [Barriers](#barriers)
5. [Propose Mechanisms](#propose-mechanisms)
6. [Message Routing and Filtering](#message-routing-and-filtering)
7. [State Persistence and Recovery](#state-persistence-and-recovery)
8. [Complete Operation Example](#complete-operation-example)

---

## Overview

SchemeShard is the core tablet responsible for schema metadata operations in YDB. It implements a sophisticated distributed transaction system where operations:
- Are divided into **parts** (sub-operations)
- Progress through multiple **states**
- Send messages to **themselves** to continue execution
- Survive crashes and restore from disk
- Coordinate with multiple shards through a 2-phase commit protocol

### Key Files

| Component | File | Lines |
|-----------|------|-------|
| Private Events | `ydb/core/tx/schemeshard/schemeshard_private.h` | 17-70 |
| Operation Structure | `ydb/core/tx/schemeshard/schemeshard__operation.h` | 10-157 |
| Side Effects | `ydb/core/tx/schemeshard/schemeshard__operation_side_effects.cpp` | Full file |
| State Types | `ydb/core/tx/schemeshard/schemeshard_subop_state_types.h` | 9-38 |
| State Base | `ydb/core/tx/schemeshard/schemeshard__operation_part.h` | 1-200 |
| Init/Restore | `ydb/core/tx/schemeshard/schemeshard__init.cpp` | Full file |

---

## Self-Continuation Pattern ("Meta" Operations)

### The Core Mechanism: TEvProgressOperation

Operations in SchemeShard send messages **to themselves** to trigger state progression. This is the "meta" operation pattern.

**Definition:** `ydb/core/tx/schemeshard/schemeshard_private.h:60-70`
```cpp
// This event is sent by a schemeshard to itself to signal that some tx state has changed
// and it should run all the actions associated with this state
struct TEvProgressOperation: public TEventLocal<TEvProgressOperation, EvProgressOperation> {
    const ui64 TxId;
    const ui32 TxPartId;

    TEvProgressOperation(ui64 txId, ui32 part)
        : TxId(txId)
        , TxPartId(part)
    {}
};
```

### How It Works

**Step 1: Operation Activation**
`ydb/core/tx/schemeshard/schemeshard__operation_side_effects.cpp:242-288`

When an operation is activated, SchemeShard sends `TEvProgressOperation` to itself:

```cpp
void TSideEffects::DoActivateOps(TSchemeShard* ss, const TActorContext& ctx) {
    for (auto txId: ActivationOps) {
        if (!ss->Operations.contains(txId)) {
            continue;
        }

        auto operation = ss->Operations.at(txId);

        // Don't activate if waiting for dependencies
        if (operation->WaitOperations.size()) {
            continue;
        }

        // Send progress message for EACH part of the operation
        for (ui32 partIdx = 0; partIdx < operation->Parts.size(); ++partIdx) {
            ctx.Send(ctx.SelfID, new TEvPrivate::TEvProgressOperation(ui64(txId), partIdx));
        }
    }
}
```

**Step 2: Message Reception**
`ydb/core/tx/schemeshard/schemeshard_impl.cpp:6067-6078`

```cpp
void TSchemeShard::Handle(TEvPrivate::TEvProgressOperation::TPtr &ev, const TActorContext &ctx) {
    const auto txId = TTxId(ev->Get()->TxId);
    if (!Operations.contains(txId)) {
        LOG_WARN_S(ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                   "Got TEvPrivate::TEvProgressOperation for unknown txId " << txId);
        return;
    }

    Y_ABORT_UNLESS(ev->Get()->TxPartId != InvalidSubTxId);
    Execute(CreateTxOperationProgress(TOperationId(txId, ev->Get()->TxPartId)), ctx);
}
```

**Step 3: State Progression**

The transaction executor runs the operation's `ProgressState()` method, which:
1. Performs work for the current state
2. May change state (via `ChangeTxState()`)
3. Returns `true` if state changed, triggering another self-message

### Example: CreateTable Self-Continuation

`ydb/core/tx/schemeshard/schemeshard__operation_create_table.cpp:215-263`

```cpp
bool TConfigureParts::ProgressState(TOperationContext& context) override {
    TTxState* txState = context.SS->FindTx(OperationId);

    txState->ClearShardsInProgress();

    // Send configuration to all shards
    for (ui32 i = 0; i < txState->Shards.size(); ++i) {
        TShardIdx shardIdx = txState->Shards[i].Idx;
        TTabletId datashardId = context.SS->ShardInfos[shardIdx].TabletID;

        // Prepare and send message to shard
        auto event = context.SS->MakeDataShardProposal(...);
        context.OnComplete.BindMsgToPipe(OperationId, datashardId, shardIdx, event.Release());
    }

    txState->UpdateShardsInProgress();
    return false;  // Don't progress yet, wait for replies
}
```

When all shard replies arrive → state changes to next → new `TEvProgressOperation` sent.

---

## Operation Stages

### State Enumeration

`ydb/core/tx/schemeshard/schemeshard_subop_state_types.h:9-38`

```cpp
enum ETxState {
    Invalid = 0,
    Waiting = 1,
    CreateParts = 2,       // Get shards from Hive
    ConfigureParts = 3,    // Send configuration to shards
    DropParts = 4,         // Send drop messages to shards
    DeleteParts = 5,       // Return shards to Hive (delete data)
    // ... other states ...
    Propose = 128,         // Propose to Coordinator (get global timestamp)
    ProposedWaitParts = 129,
    // ... more states ...
    Done = 240,
    Aborted = 250,
};
```

### Typical Operation Flow (CreateTable Example)

**State Progression:** `ydb/core/tx/schemeshard/schemeshard__operation_create_table.cpp:380-398`

```cpp
TTxState::ETxState NextState(TTxState::ETxState state) const override {
    switch (state) {
    case TTxState::Waiting:
    case TTxState::CreateParts:
        return TTxState::ConfigureParts;
    case TTxState::ConfigureParts:
        return TTxState::Propose;
    case TTxState::Propose:
        return TTxState::ProposedWaitParts;
    case TTxState::ProposedWaitParts:
        return TTxState::Done;
    default:
        return TTxState::Invalid;
    }
}
```

### Stage Details

#### 1. **CreateParts** - Shard Creation
`ydb/core/tx/schemeshard/schemeshard__operation_common.cpp:174-285`

- Sends `TEvCreateTablet` to Hive for each shard
- Receives `TEvCreateTabletReply` with tablet IDs
- Tracks progress in `txState->ShardsInProgress`
- When all created → advances to `ConfigureParts`

**Key Code:**
```cpp
bool TCreateParts::HandleReply(TEvHive::TEvCreateTabletReply::TPtr& ev, TOperationContext& context) {
    auto shardIdx = TShardIdx(ev->Get()->Record.GetOwner(), ...);
    auto tabletId = TTabletId(ev->Get()->Record.GetTabletID());

    TShardInfo& shardInfo = context.SS->ShardInfos.at(shardIdx);
    shardInfo.TabletID = tabletId;

    txState.ShardsInProgress.erase(shardIdx);

    // If all datashards have been created
    if (txState.ShardsInProgress.empty()) {
        context.SS->ChangeTxState(db, OperationId, TTxState::ConfigureParts);
        return true;  // State changed - trigger progression
    }

    return false;
}
```

#### 2. **ConfigureParts** - Shard Configuration
`ydb/core/tx/schemeshard/schemeshard__operation_create_table.cpp:215-263`

- Sends `TEvProposeTransaction` to each DataShard with schema
- Receives `TEvProposeTransactionResult`
- When all configured → advances to `Propose`

#### 3. **Propose** - Coordinator Proposal
`ydb/core/tx/schemeshard/schemeshard__operation_create_table.cpp:353-373`

```cpp
bool TPropose::ProgressState(TOperationContext& context) override {
    TTxState* txState = context.SS->FindTx(OperationId);

    TSet<TTabletId> shardSet;
    for (const auto& shard : txState->Shards) {
        TTabletId tablet = context.SS->ShardInfos.at(shard.Idx).TabletID;
        shardSet.insert(tablet);
    }

    // Propose to coordinator to get global plan step
    context.OnComplete.ProposeToCoordinator(OperationId, txState->TargetPathId,
                                           txState->MinStep, shardSet);
    return false;
}
```

Receives `TEvOperationPlan` with step ID → marks path as created at step → advances to `ProposedWaitParts`.

#### 4. **ProposedWaitParts** - Wait for Schema Application
- Waits for `TEvSchemaChanged` from all shards
- Confirms shards applied the schema
- When all confirm → advances to `Done`

---

## Barriers

Barriers block operation parts from completing until all parts reach the barrier point.

### Implementation

`ydb/core/tx/schemeshard/schemeshard__operation.h:119-146`

```cpp
// Barrier registration
void RegisterBarrier(TSubTxId partId, const TString& name) {
    Barriers[name].insert(partId);
    Y_ABORT_UNLESS(Barriers.size() == 1);  // Only one barrier at a time
}

bool HasBarrier() const {
    Y_ABORT_UNLESS(Barriers.size() <= 1);
    return Barriers.size() == 1;
}

bool IsDoneBarrier() const {
    Y_ABORT_UNLESS(Barriers.size() <= 1);

    for (const auto& [_, subTxIds] : Barriers) {
        for (const auto blocked : subTxIds) {
            Y_VERIFY_S(!DoneParts.contains(blocked),
                      "part is blocked and done: " << blocked);
        }
        // Barrier complete when: blocked parts + done parts = all parts
        return subTxIds.size() + DoneParts.size() == Parts.size();
    }

    return false;
}

void DropBarrier(const TString& name) {
    Y_ABORT_UNLESS(IsDoneBarrier());
    Barriers.erase(name);
}
```

### Barrier Processing

`ydb/core/tx/schemeshard/schemeshard__operation_side_effects.cpp:1084-1138`

```cpp
void TSideEffects::DoCheckBarriers(TSchemeShard *ss, TTransactionContext &txc,
                                   const TActorContext &ctx) {
    for (auto& txId : touchedOperations) {
        auto& operation = ss->Operations.at(txId);

        if (!operation->HasBarrier() || !operation->IsDoneBarrier()) {
            continue;
        }

        auto name = operation->Barriers.begin()->first;
        const auto& blockedParts = operation->Barriers.begin()->second;

        LOG_NOTICE_S(ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                     "All parts have reached barrier, tx: " << txId
                     << ", done: " << operation->DoneParts.size()
                     << ", blocked: " << blockedParts.size());

        // Send TEvCompleteBarrier to all blocked parts
        THolder<TEvPrivate::TEvCompleteBarrier> msg =
            MakeHolder<TEvPrivate::TEvCompleteBarrier>(txId, name);

        for (auto& partId: blockedParts) {
            // Call HandleReply on each blocked part
            operation->Parts.at(partId)->HandleReply(personalEv, context);
        }

        operation->DropBarrier(name);
    }
}
```

### Real Example: Drop Table with Multiple Indexes

When dropping a table with indexes, all index drop operations must complete before table drop:

1. Each index drop registers barrier "wait_all_indexes_dropped"
2. As index drops complete, they mark as done
3. When last index completes barrier → `TEvCompleteBarrier` sent
4. Table drop proceeds

---

## Propose Mechanisms

### Two Types of Propose

1. **Propose to Coordinator** - Get global timestamp for schema operation
2. **Propose to Shards** - Send transactions to data shards

### Propose to Coordinator

`ydb/core/tx/schemeshard/schemeshard__operation_side_effects.cpp:313-343`

```cpp
void TSideEffects::ExpandCoordinatorProposes(TSchemeShard* ss, const TActorContext& ctx) {
    TSet<TTxId> touchedTxIds;

    // Collect all propose requests
    for (auto& rec: CoordinatorProposes) {
        TOperationId opId;
        TPathId pathId;
        TStepId minStep;
        std::tie(opId, pathId, minStep) = rec;

        // Mark part as ready to propose
        ss->Operations.at(opId.GetTxId())->ProposePart(opId.GetSubTxId(), pathId, minStep);
        touchedTxIds.insert(opId.GetTxId());
    }

    // If operation ready (all parts propose registered) → actually propose
    for (TTxId txId: touchedTxIds) {
        TOperation::TPtr operation = ss->Operations.at(txId);
        if (operation->IsReadyToPropose(ctx)) {
            operation->DoPropose(ss, *this, ctx);
        }
    }
}
```

**The actual proposal:** `ydb/core/tx/schemeshard/schemeshard__operation.cpp:1742-1796`

```cpp
void TOperation::DoPropose(TSchemeShard* ss, TSideEffects& sideEffects,
                           const TActorContext& ctx) const {
    // Group proposes by coordinator
    THashMap<TActorId, TVector<TProposeRec>> byCoordinator;

    for (const auto& propose : Proposes) {
        TSubTxId partId;
        TPathId pathId;
        TStepId minStep;
        std::tie(partId, pathId, minStep) = propose;

        TActorId coordinator = ss->SelectCoordinator(...);
        byCoordinator[coordinator].push_back(propose);
    }

    // Send TEvProposeTransaction to each coordinator
    for (const auto& [coordinator, proposes] : byCoordinator) {
        auto ev = MakeHolder<TEvTxProcessing::TEvProposeTransaction>(...);
        sideEffects.Send(coordinator, ev.Release());
    }
}
```

### Propose to Shards

`ydb/core/tx/schemeshard/schemeshard__operation_create_table.cpp:230-259`

```cpp
for (ui32 i = 0; i < txState->Shards.size(); ++i) {
    TShardIdx shardIdx = txState->Shards[i].Idx;
    TTabletId datashardId = context.SS->ShardInfos[shardIdx].TabletID;

    auto seqNo = context.SS->StartRound(*txState);

    // Create transaction for this shard
    NKikimrTxDataShard::TFlatSchemeTransaction tx;
    auto tableDesc = tx.MutableCreateTable();
    context.SS->FillSeqNo(tx, seqNo);
    context.SS->FillTableDescription(txState->TargetPathId, i,
                                    NEW_TABLE_ALTER_VERSION, tableDesc);

    auto event = context.SS->MakeDataShardProposal(txState->TargetPathId, OperationId,
                                                   tx.SerializeAsString(), context.Ctx);

    // Bind message to pipe - will be resent on reconnect
    context.OnComplete.BindMsgToPipe(OperationId, datashardId, shardIdx, event.Release());
}
```

**Key difference:** Messages to shards are **bound to pipes** and automatically resent on disconnect.

---

## Message Routing and Filtering

### The Problem

An operation might receive messages:
- From previous states (should ignore)
- For different operations on same shards (should ignore)
- Out of order (should handle gracefully)

### Solution 1: Message Routing by Tablet/Shard

`ydb/core/tx/schemeshard/schemeshard__operation.h:37-38`

```cpp
THashMap<TTabletId, TSubTxId> RelationsByTabletId;
THashMap<TShardIdx, TSubTxId> RelationsByShardIdx;
```

**Registration:** `ydb/core/tx/schemeshard/schemeshard__operation_side_effects.cpp:725-760`

```cpp
void TSideEffects::DoRegisterRelations(TSchemeShard *ss, const TActorContext &ctx) {
    for (auto& rec: RelationsByTabletId) {
        TOperationId opId;
        TTabletId tablet;
        std::tie(opId, tablet) = rec;

        if (auto opPPtr = ss->Operations.FindPtr(opId.GetTxId())) {
           (*opPPtr)->RegisterRelationByTabletId(opId.GetSubTxId(), tablet, ctx);
        }
    }

    for (auto& rec: RelationsByShardIdx) {
        TOperationId opId;
        TShardIdx shardIdx;
        std::tie(opId, shardIdx) = rec;

        if (auto opPPtr = ss->Operations.FindPtr(opId.GetTxId())) {
           (*opPPtr)->RegisterRelationByShardIdx(opId.GetSubTxId(), shardIdx, ctx);
        }
    }
}
```

**Message Routing Example:**
```cpp
void TSchemeShard::Handle(TEvDataShard::TEvProposeTransactionResult::TPtr& ev,
                         const TActorContext& ctx) {
    auto tabletId = TTabletId(ev->Get()->Record.GetTabletId());

    // Find which operation part handles this tablet
    TSubTxId partId = Operations.at(txId)->FindRelatedPartByTabletId(tabletId, ctx);
    if (partId == InvalidSubTxId) {
        LOG_WARN_S(ctx, "Got reply from unknown tablet");
        return;  // IGNORE!
    }

    Execute(CreateTxOperationReply(TOperationId(txId, partId), ev), ctx);
}
```

### Solution 2: IgnoreMessages Declaration

`ydb/core/tx/schemeshard/schemeshard__operation_create_table.cpp:197-200`

Each state declares which messages it should IGNORE:

```cpp
TConfigureParts(TOperationId id)
    : OperationId(id)
{
    // Ignore TEvCreateTabletReply in ConfigureParts state
    // (relevant only in CreateParts state)
    IgnoreMessages(DebugHint(), {TEvHive::TEvCreateTabletReply::EventType});
}
```

**Implementation:** When message arrives for operation in wrong state, it's logged and dropped.

### Solution 3: Pipe-Bound Messages (Automatic Resend)

`ydb/core/tx/schemeshard/schemeshard__operation.h:21-35`

```cpp
struct TPreSerializedMessage {
    ui32 Type;
    TIntrusivePtr<TEventSerializedData> Data;
    TOperationId OpId;
};

THashMap<TTabletId, TMap<TPipeMessageId, TPreSerializedMessage>> PipeBindedMessages;
```

Messages bound to pipes are:
- Stored in operation state
- Automatically resent on pipe reconnect
- Acknowledged when shard confirms receipt
- Cleaned up when operation completes

**Binding:** `ydb/core/tx/schemeshard/schemeshard__operation_side_effects.cpp:647-689`

```cpp
void TSideEffects::DoBindMsg(TSchemeShard *ss, const TActorContext &ctx) {
    for (auto& rec: BindedMessages) {
        TOperationId opId;
        TTabletId tablet;
        TPipeMessageId cookie;
        THolder<::NActors::IEventBase> message;
        std::tie(opId, tablet, cookie, message) = rec;

        TOperation::TPtr operation = ss->Operations.at(opId.GetTxId());

        // Serialize message
        TAllocChunkSerializer serializer;
        message->SerializeToArcadiaStream(&serializer);
        TIntrusivePtr<TEventSerializedData> data = serializer.Release(...);

        // Store for potential resend
        operation->PipeBindedMessages[tablet][cookie] =
            TOperation::TPreSerializedMessage(msgType, data, opId);

        // Send via pipe cache (will resend on reconnect)
        ss->PipeClientCache->Send(ctx, ui64(tablet), msgType, data, cookie.second);
    }
}
```

**Acknowledgement:** `ydb/core/tx/schemeshard/schemeshard__operation_side_effects.cpp:692-723`

```cpp
void TSideEffects::DoBindMsgAcks(TSchemeShard *ss, const TActorContext &ctx) {
    for (auto& ack: BindedMessageAcks) {
        TOperationId opId;
        TTabletId tablet;
        TPipeMessageId cookie;
        std::tie(opId, tablet, cookie) = ack;

        TOperation::TPtr operation = ss->Operations.at(opId.GetTxId());

        // Remove from pending resend map
        if (operation->PipeBindedMessages.contains(tablet)) {
            if (operation->PipeBindedMessages[tablet].contains(cookie)) {
                operation->PipeBindedMessages[tablet].erase(cookie);
            }
        }
    }
}
```

---

## State Persistence and Recovery

### Saving State

**State Change Persistence:** `ydb/core/tx/schemeshard/schemeshard_impl.cpp:2690-2704`

```cpp
void TSchemeShard::ChangeTxState(NIceDb::TNiceDb& db, const TOperationId opId,
                                TTxState::ETxState newState) {
    Y_VERIFY_S(FindTx(opId), "Unknown TxId " << opId.GetTxId());

    LOG_INFO_S(ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
               "Change state for txid " << opId << " "
               << (int)TxInFlight[opId].State << " -> " << (int)newState);

    FindTx(opId)->State = newState;

    // PERSIST to database
    db.Table<Schema::TxInFlightV2>().Key(opId.GetTxId(), opId.GetSubTxId()).Update(
        NIceDb::TUpdate<Schema::TxInFlightV2::State>(newState));
}
```

**Database Schema:**
- Table: `TxInFlightV2`
- Key: `(TxId, SubTxId)`
- Columns: `State`, `TxType`, `TargetPathId`, `MinStep`, `PlanStep`, etc.

### Restoring Operations on Restart

**Init Transaction:** `ydb/core/tx/schemeshard/schemeshard__init.cpp:31-33`

During `TTxInit`, SchemeShard:
1. Loads all paths from disk
2. Loads all in-flight transactions
3. **Restores operation objects** for each transaction
4. Sends `TEvProgressOperation` to continue where left off

**Restoration Logic:** `ydb/core/tx/schemeshard/schemeshard__init.cpp:3734`

```cpp
// For each TxState loaded from DB
ISubOperation::TPtr part = operation->RestorePart(txState.TxType, txState.State, context);
```

**RestorePart Implementation:** `ydb/core/tx/schemeshard/schemeshard__operation.cpp:1031-1130`

```cpp
ISubOperation::TPtr TOperation::RestorePart(TTxState::ETxType txType,
                                           TTxState::ETxState txState,
                                           TOperationContext& context) const {
    switch (txType) {
    case TTxState::TxCreateTable:
        return CreateNewTable(NextPartId(), txState);
    case TTxState::TxAlterTable:
        return CreateAlterTable(NextPartId(), txState);
    case TTxState::TxSplitTablePartition:
    case TTxState::TxMergeTablePartition:
        return CreateSplitMerge(NextPartId(), txState);
    case TTxState::TxDropTable:
        return CreateDropTable(NextPartId(), txState);
    // ... all operation types ...
    }
}
```

**State Selection:** Each operation type selects appropriate state handler:

```cpp
TSubOperationState::TPtr SelectStateFunc(TTxState::ETxState state) override {
    switch (state) {
    case TTxState::CreateParts:
        return MakeHolder<TCreateParts>(OperationId);
    case TTxState::ConfigureParts:
        return MakeHolder<TConfigureParts>(OperationId);
    case TTxState::Propose:
        return MakeHolder<TPropose>(OperationId);
    case TTxState::ProposedWaitParts:
        return MakeHolder<NTableState::TProposedWaitParts>(OperationId);
    case TTxState::Done:
        return MakeHolder<TDone>(OperationId);
    default:
        return nullptr;
    }
}
```

### Crash Recovery Flow

```
┌─────────────────────┐
│  SchemeShard Crash  │
└──────────┬──────────┘
           │
           v
┌─────────────────────┐
│  TTxInit Execute    │
│  - Load Paths       │
│  - Load TxInFlight  │
└──────────┬──────────┘
           │
           v
┌─────────────────────────────────┐
│  For each TxState in DB:        │
│  1. Create TOperation object    │
│  2. RestorePart(txType, state)  │
│  3. SelectStateFunc(state)      │
└──────────┬──────────────────────┘
           │
           v
┌─────────────────────────────────┐
│  Send TEvProgressOperation      │
│  to self for each operation     │
└──────────┬──────────────────────┘
           │
           v
┌─────────────────────────────────┐
│  Operation continues from       │
│  saved state as if nothing      │
│  happened!                      │
└─────────────────────────────────┘
```

**Key Point:** Operations are **idempotent** - they can safely re-execute the same state multiple times (e.g., resending messages to shards that were already received).

---

## Complete Operation Example

Let's trace a **CreateTable** operation from start to finish.

### Request Received

```
User → SchemeShard: TEvModifySchemeTransaction(CreateTable)
```

### Step 1: Propose (Validation)

`ydb/core/tx/schemeshard/schemeshard__operation_create_table.cpp:433-456`

```cpp
THolder<TProposeResponse> Propose(const TString& owner, TOperationContext& context) override {
    // Validate request
    // Check permissions
    // Reserve resources
    // Create TTableInfo

    TTxState& txState = context.SS->CreateTx(OperationId, TTxState::TxCreateTable,
                                             targetPath->PathId);
    txState.State = TTxState::CreateParts;

    // Persist to DB
    NIceDb::TNiceDb db(context.GetDB());
    context.SS->PersistTxState(db, OperationId);

    // Schedule activation
    context.OnComplete.ActivateOperation(OperationId.GetTxId());

    return result;  // StatusAccepted
}
```

**Persisted:** TxState(TxId, State=CreateParts, Type=TxCreateTable)

### Step 2: CreateParts State

**Message to self:** `TEvProgressOperation(TxId, 0)`

**ProgressState:**
```cpp
for (each partition) {
    auto ev = CreateEvCreateTablet(path, shardIdx, context);
    context.OnComplete.BindMsgToPipe(OperationId, hiveTabletId, shardIdx, ev.Release());
}
```

**Persisted:** Shard mappings

### Step 3: Wait for Hive Replies

```
Hive → SchemeShard: TEvCreateTabletReply(shardIdx=1, tabletId=1001)
Hive → SchemeShard: TEvCreateTabletReply(shardIdx=2, tabletId=1002)
...
```

**HandleReply:**
```cpp
shardInfo.TabletID = tabletId;
txState.ShardsInProgress.erase(shardIdx);

if (txState.ShardsInProgress.empty()) {
    context.SS->ChangeTxState(db, OperationId, TTxState::ConfigureParts);
    return true;  // Triggers TEvProgressOperation
}
```

**Persisted:** TabletID mappings, State=ConfigureParts

### Step 4: ConfigureParts State

**Message to self:** `TEvProgressOperation(TxId, 0)`

**ProgressState:**
```cpp
for (each shard) {
    NKikimrTxDataShard::TFlatSchemeTransaction tx;
    tx.MutableCreateTable()->CopyFrom(tableSchema);

    auto event = MakeDataShardProposal(tx);
    context.OnComplete.BindMsgToPipe(OperationId, datashardId, shardIdx, event.Release());
}
```

### Step 5: Wait for DataShard Configuration

```
DataShard[1001] → SchemeShard: TEvProposeTransactionResult(Status=Prepared)
DataShard[1002] → SchemeShard: TEvProposeTransactionResult(Status=Prepared)
...
```

**HandleReply:**
```cpp
if (all shards prepared) {
    context.SS->ChangeTxState(db, OperationId, TTxState::Propose);
    return true;
}
```

**Persisted:** State=Propose

### Step 6: Propose to Coordinator

**Message to self:** `TEvProgressOperation(TxId, 0)`

**ProgressState:**
```cpp
context.OnComplete.ProposeToCoordinator(OperationId, pathId, minStep, shardSet);
```

```
SchemeShard → Coordinator: TEvProposeTransaction(TxId, shards=[1001,1002])
```

### Step 7: Receive Plan Step

```
Coordinator → SchemeShard: TEvPlanStep(step=12345, txId=...)
```

**HandleReply:**
```cpp
path->StepCreated = step;
context.SS->PersistCreateStep(db, pathId, step);
context.SS->ChangeTxState(db, OperationId, TTxState::ProposedWaitParts);
```

**Persisted:** StepCreated=12345, State=ProposedWaitParts

### Step 8: Wait for Schema Application

```
DataShard[1001] → SchemeShard: TEvSchemaChanged(txId, ...)
DataShard[1002] → SchemeShard: TEvSchemaChanged(txId, ...)
```

**HandleReply:**
```cpp
if (all shards confirmed schema changed) {
    context.OnComplete.DoneOperation(OperationId);
}
```

### Step 9: Publish to SchemeBoard

```cpp
context.OnComplete.PublishToSchemeBoard(OperationId, pathId);
```

**Persisted:** Operation removed from TxInFlightV2

### Step 10: Done

```
SchemeShard → User: TEvModifySchemeTransactionResult(Status=Success, Step=12345)
```

---

## Summary

### Key Takeaways

1. **Self-Continuation Pattern**
   - Operations send `TEvProgressOperation` to themselves
   - Enables async, resumable operation flow
   - Survives crashes via state persistence

2. **State Machine Architecture**
   - Clear stages: CreateParts → ConfigureParts → Propose → ProposedWaitParts → Done
   - Each state has dedicated handler class
   - State changes persisted to DB immediately

3. **Barriers**
   - Synchronize multiple operation parts
   - Block completion until all parts reach barrier
   - Send `TEvCompleteBarrier` when barrier condition met

4. **Dual Propose Mechanism**
   - **To Coordinator:** Get global timestamp (linearizability)
   - **To Shards:** Distribute schema changes (2PC)

5. **Message Routing & Filtering**
   - Route by TabletId/ShardIdx → correct operation part
   - Ignore messages from old states via `IgnoreMessages()`
   - Pipe-bound messages auto-resend on disconnect

6. **Crash Recovery**
   - All state persisted: TxInFlight table
   - On restart: `RestorePart()` recreates operation objects
   - Resume from last persisted state
   - Idempotent operations handle duplicate messages

### Architecture Benefits

- **Fault Tolerance:** Operations survive SchemeShard crashes
- **Scalability:** Async message-passing, no blocking
- **Debuggability:** Clear state progression, extensive logging
- **Correctness:** 2PC ensures schema consistency across shards
- **Flexibility:** Easy to add new operation types and states

This architecture is a sophisticated example of distributed transaction management in a replicated, fault-tolerant system.
