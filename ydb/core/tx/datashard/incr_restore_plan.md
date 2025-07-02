# YDB Incremental Restore Fix - Updated Status & Coordination Details

## 🎯 **Current Status Summary**

### ✅ **Major Progress Completed**
- **Phases 1-7**: Extensive debugging and root cause analysis completed
- **Path Mapping Bug**: Fixed source/destination path assignment in schema transactions  
- **Backup Table Discovery**: Fixed lookup logic to find tables within timestamped backup entries
- **Schema Transactions**: Now being sent and accepted correctly by DataShard ("Prepared scheme transaction")
- **Event-based logic removed**: All direct event sending and `RestoreRequests` tracking removed from scan logic
- **SchemeShard operation registration**: Scan logic now creates and registers operations using the correct infrastructure (TOperation + suboperation pattern)
- **Build errors fixed**: All build errors related to event-based logic and operation registration are resolved
- **Unused/undeclared variable errors fixed**: All references to removed variables (e.g., `op`) are commented out or removed

### ❌ **Current Test Failure**
- **Incremental restore does not apply data changes**: The test expects only the incremental backup data to be present (e.g., key=2, value=2000), but the full table is restored with old values. No data is transferred or merged by DataShard during restore.
- **Root cause**: The transaction created by the scan logic is not providing the DataShard with the necessary information to perform the data transfer for incremental restore. The operation is registered and completed, but no data is actually copied or merged.

### 🔄 **In Progress**
- **Parameter wiring**: Correct source/destination path parameters are now wired into `TxRestoreIncrementalBackupAtTable` operations. Each operation receives the correct backup and destination table path IDs from scan logic.
- **Transaction construction**: Need to ensure all required fields for incremental data restore are set, so the DataShard can perform the actual data transfer.

### 🧪 **Next Steps**
- [ ] Fix transaction construction in scan logic to provide all required fields for incremental restore
- [ ] Test that schema transactions now go through the full SchemeShard operation flow
- [ ] Confirm DataShards receive transactions with plan steps and execution units are triggered
- [ ] Validate that data changes are applied as expected
- [ ] Test with multiple incremental backups for sequential operation

---

## 📝 **Recent Progress Log**
- Removed all event-based infrastructure and direct DataShard event sending from scan logic
- Replaced with operation-based registration using SchemeShard's operation queue
- Fixed all build errors, including type mismatches and unused/undeclared variable errors
- Wired correct source/destination path parameters into operation construction
- Updated this plan to reflect current status and next steps
- **Tested integration: DataShard receives and completes operation, but no data is transferred or merged**
- **Identified root cause: Transaction is missing required fields for DataShard to perform incremental restore**
- **Checked logs and test output**: Confirmed that DataShard receives and completes the operation, but no data is transferred or merged. The test output and logs are now documented for the next agent to review for further debugging.
- **Next agent action**: Please review the attached logs and test output to further diagnose why the DataShard is not applying incremental data changes. Focus on transaction field wiring and DataShard execution unit triggers. For this iteration they are available here: /home/innokentii/ydbwork2/ydb/ydb/core/tx/datashard/logs

---

## 🏗️ **ARCHITECTURAL SOLUTION IDENTIFIED**

### **Key Insight**: Existing Infrastructure Available
**File**: `schemeshard__operation_create_restore_incremental_backup.cpp` already implements proper `TxRestoreIncrementalBackupAtTable` operation with correct transaction coordination via `context.OnComplete.BindMsgToPipe()`.

**CORRECT APPROACH**: Scan logic should trigger proper SchemeShard operations instead of creating schema transactions directly.

### **Current Implementation Problem**
**File**: `schemeshard_incremental_restore_scan.cpp` lines 66-85 in `Complete()` method
```cpp
// CURRENT (INCORRECT) - Bypasses SchemeShard operation infrastructure:
auto event = MakeDataShardProposal(pathId, txId, restoreBackup);
Self->PipeClientCache->Send(datashardId, event.Release());
```

## 🔧 **DETAILED IMPLEMENTATION PLAN**

### **Step 8.3: Proper SchemeShard Operation Integration** ⚠️ **CURRENT FOCUS**

#### **STEP 1: Modify Scan Logic to Trigger Operations**
- [x] In `schemeshard_incremental_restore_scan.cpp`, replaced event-based logic in `TTxProgress::Complete()` with calls to `Self->Execute(CreateRestoreIncrementalBackupAtTable(...), ctx)` for each required restore operation.
- [x] Removed `RestoreRequests` tracking and all direct `PipeClientCache->Send()` calls to DataShards.
- [x] The scan logic now triggers proper SchemeShard operations, letting the operation framework handle transaction coordination and plan steps.

#### **STEP 2: Remove Event-Based Infrastructure**
- [x] Removed `MakeDataShardProposal()` calls that bypass SchemeShard transaction flow
- [x] Removed direct `PipeClientCache->Send()` calls to DataShards
- [x] Cleaned up event handling that duplicated operation functionality

#### **STEP 3: Integrate with Existing Operation Framework**
- [x] Now using existing `schemeshard__operation_create_restore_incremental_backup.cpp` operations
- [x] Operations use proper `context.OnComplete.BindMsgToPipe()` for transaction coordination
- [x] SchemeShard's operation infrastructure now handles transaction lifecycle and plan steps

#### **STEP 4: Update Operation Parameters**
- [~] Ensure `TxRestoreIncrementalBackupAtTable` operations receive correct source/destination paths (in progress: wiring from op's TxBody or fields)
- [~] Pass backup table paths and destination table paths from scan logic discovery (in progress)
- [x] Maintain proper operation tracking and completion handling (operation now registered via SchemeShard operation queue)

#### **STEP 5: Test Complete Architectural Fix**
- [ ] Verify schema transactions now go through proper SchemeShard transaction execution flow
- [ ] Confirm DataShards receive transactions with proper plan steps
- [ ] Validate that execution units are triggered and data changes are applied
- [ ] Test with multiple incremental backups to ensure sequential processing works

## 🔄 **TRANSACTION COORDINATION REQUIREMENTS**

### **Why Current Event-Based Approach Fails**
```cpp
// BROKEN FLOW:
PipeClientCache->Send() → DataShard receives transaction → Accepts but no plan steps → Execution units never triggered
```

### **Required Operation-Based Flow**
```cpp
// CORRECT FLOW:
Self->Execute(CreateTxRestoreIncrementalBackupAtTable()) → SchemeShard operation → context.OnComplete.BindMsgToPipe() → DataShard with plan steps → Execution units triggered
```

### **Existing Operation Infrastructure Pattern**
**From**: `schemeshard__operation_create_restore_incremental_backup.cpp`
```cpp
// PROVEN COORDINATION PATTERN:
context.OnComplete.BindMsgToPipe(OperationId, datashardId, pathIdx, event.Release());
```

### **Multiple Table Coordination**
For multiple incremental backups, the operation framework handles:
- **Sequential Processing**: Each backup table gets separate operation with proper coordination
- **Operation Tracking**: SchemeShard operation infrastructure waits for all datashard transactions
- **Response Handling**: Processes results through proper operation lifecycle

## 🎯 **IMMEDIATE ACTIONS REQUIRED**

### **Phase 8: Implement Proper SchemeShard Operation Integration** (CURRENT FOCUS)

#### **Priority 1: Replace Event-Based Approach** ⚠️ **URGENT**
**Location**: `schemeshard_incremental_restore_scan.cpp` Complete() method
**Change**: Replace direct schema transaction creation with operation triggering

#### **Priority 2: Leverage Existing Operations**
**Strategy**: Use existing `TxRestoreIncrementalBackupAtTable` operation instead of duplicating coordination logic

#### **Priority 3: Verification Points**
1. **Operation Triggering**: Confirm operations are created and executed properly
2. **Transaction Flow**: Verify proper SchemeShard operation lifecycle
3. **Execution Units**: Confirm `TCreateIncrementalRestoreSrcUnit::Run()` is called
4. **Data Changes**: Verify value changes from `(2,20)` to `(2,2000)`

**CRITICAL UNDERSTANDING**: 
- The `TTxProgress` scan logic should NOT create schema transactions directly
- The existing operation infrastructure already has proper transaction coordination
- Solution is to trigger operations, not fix event sending
- This architectural change ensures proper SchemeShard transaction lifecycle

**Expected Result**: After implementing operation-based approach, execution units will be triggered and incremental restore will apply data changes correctly.