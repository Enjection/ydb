# YDB Incremental Restore Fix - Comprehensive Implementation Plan

## 🎯 **CRITICAL DISCOVERY: Proper SchemeShard Operation Pattern**

### 🔍 **Root Cause Analysis Complete**
After extensive investigation, the fundamental issue has been identified:

**WRONG APPROACH**: Direct operation creation via `Self->Execute(CreateRestoreIncrementalBackupAtTable())`
- `CreateRestoreIncrementalBackupAtTable()` returns `ISubOperation::TPtr` (operation objects)  
- `Self->Execute()` expects `ITransaction*` objects (transaction classes like `TTxProgress`)
- This API mismatch was causing compilation errors and incorrect execution flow

**CORRECT APPROACH**: Transaction-based flow with Propose messages
- Use `TTxProgress` transaction classes that inherit from `ITransaction`
- Create "Propose" messages that become `TEvModifySchemeTransaction` events
- Send propose messages to SchemeShard via `Send(Self->SelfId(), propose)`
- Let SchemeShard's operation infrastructure handle the lifecycle

### 📚 **Pattern Discovery from Export System**
Analysis of `schemeshard_export__create.cpp` revealed the correct pattern:

1. **Transaction Allocation**: `Send(Self->TxAllocatorClient, new TEvTxAllocatorClient::TEvAllocate())`
2. **Propose Creation**: Create propose functions like `MkDirPropose()`, `BackupPropose()`, `CopyTablesPropose()`
3. **Send to SchemeShard**: `Send(Self->SelfId(), propose)` where propose returns `THolder<TEvModifySchemeTransaction>`
4. **Operation Processing**: SchemeShard creates proper operations and handles coordination
5. **Result Handling**: `TEvModifySchemeTransactionResult` events processed in transaction

### ✅ **Previous Architectural Progress**
- **Event-based logic removed**: All direct `PipeClientCache->Send()` calls eliminated
- **Operation-based foundation**: Infrastructure for operation registration in place
- **Parameter wiring**: Source/destination path discovery working correctly
- **Build compatibility**: Fixed compilation errors from previous approaches

### ❌ **Current API Mismatch Issue**
- **Problem**: Trying to execute operation objects instead of transaction objects
- **Evidence**: `Self->Execute(CreateRestoreIncrementalBackupAtTable())` compilation errors
- **Solution**: Need to create proper propose functions and transaction flow

---

## 🏗️ **COMPREHENSIVE IMPLEMENTATION PLAN**

### **Phase 9: Create Proper Propose Infrastructure** ⚠️ **IMMEDIATE FOCUS**

#### **Step 1: Create IncrementalRestorePropose Function**
**Location**: Create new file `schemeshard_incremental_restore_flow_proposals.cpp` or add to existing proposals file

**Pattern**: Following `BackupPropose()`, `MkDirPropose()`, `CopyTablesPropose()` from export system
```cpp
THolder<TEvSchemeShard::TEvModifySchemeTransaction> IncrementalRestorePropose(
    TSchemeShard* ss, 
    TTxId txId, 
    const TPathId& sourceBackupPathId,
    const TPathId& destinationTablePathId
) {
    // Create TEvModifySchemeTransaction with proper transaction structure
    // Set transaction type, source/destination paths, operation parameters
    // Return event that SchemeShard can process through operation infrastructure
}
```

#### **Step 2: Update TTxProgress Transaction Pattern**
**Location**: `schemeshard_incremental_restore_scan.cpp`

**Current (BROKEN)**:
```cpp
Self->Execute(CreateRestoreIncrementalBackupAtTable(newOperationId, newTx), ctx)
```

**Target (CORRECT)**:
```cpp
// In OnRunIncrementalRestore() and OnPipeRetry():
1. AllocateTxId() -> Send TEvAllocate request
2. Wait for TEvAllocateResult 
3. Call IncrementalRestorePropose() with allocated txId
4. Send(Self->SelfId(), propose) 
5. Wait for TEvModifySchemeTransactionResult
6. Handle completion and state transitions
```

#### **Step 3: Implement Transaction Lifecycle Management**
**Required Methods in TTxProgress**:
- `AllocateTxId()` - Request transaction ID allocation
- `OnAllocateResult()` - Handle allocated transaction ID and send propose  
- `OnModifyResult()` - Handle propose result and track operation state
- `SubscribeTx()` - Subscribe to transaction completion notifications
- `OnNotifyResult()` - Handle transaction completion and trigger next steps

#### **Step 4: Create Proper Transaction Structure**
**In IncrementalRestorePropose()**:
```cpp
auto transaction = MakeTransaction<TKqpSchemeOperation>();
transaction->SetTransactionId(txId);
auto* restoreOp = transaction->MutableRestoreMultipleIncrementalBackups();
restoreOp->AddSrc()->SetSrcPathId(sourceBackupPathId.LocalPathId);
restoreOp->AddSrc()->SetDstPathId(destinationTablePathId.LocalPathId);
// Set other required fields for incremental restore operation
```

### **Phase 10: Fix Scan Logic Integration** 

#### **Step 1: Remove Direct Operation Creation**
**Location**: `schemeshard_incremental_restore_scan.cpp` lines 304-400

**Remove**:
- All calls to `CreateRestoreIncrementalBackupAtTable()`
- Direct operation registration via `Self->Operations[newOperationId] = op`  
- Manual operation and suboperation creation

**Replace With**:
- Transaction allocation requests
- Propose message creation and sending
- Proper state tracking for transaction lifecycle

#### **Step 2: Fix Transaction Event Handlers**
**Update**:
- `Handle(TEvPrivate::TEvRunIncrementalRestore::TPtr& ev)` to use transaction pattern
- `CreateTxProgressIncrementalRestore()` to return proper `TTxProgress` transaction
- `CreatePipeRetryIncrementalRestore()` to use transaction pattern

#### **Step 3: Coordinate with Main Operation**
**Strategy**: Ensure scan logic doesn't conflict with main restore operation
- Check if operations already exist before creating new ones
- Proper timing coordination between scan results and operation execution
- Avoid duplicate operation creation that overwrites working operations

### **Phase 11: Test and Validate Complete Fix**

#### **Step 1: Build and Compilation Test**
- Verify all API mismatches resolved  
- Confirm proper transaction inheritance and method signatures
- Test that propose functions return correct event types

#### **Step 2: Integration Testing**
- Verify transaction allocation and propose sending works
- Confirm SchemeShard processes `TEvModifySchemeTransaction` correctly
- Check that proper operations are created by SchemeShard infrastructure

#### **Step 3: End-to-End Data Transfer Validation**
- Test that DataShards receive transactions with plan steps
- Verify execution units (`TCreateIncrementalRestoreSrcUnit`) are triggered
- Confirm incremental backup data is applied (test expects value change from 20 to 2000)

#### **Step 4: Multiple Backup Testing**
- Test sequential processing of multiple incremental backups
- Verify proper operation ordering and coordination
- Confirm no operation duplication or timing conflicts

---

## 🔄 **CRITICAL PATTERN COMPARISON**

### **Export System Pattern (WORKING)**
```cpp
// 1. Transaction allocation
Send(Self->TxAllocatorClient, new TEvTxAllocatorClient::TEvAllocate(), 0, exportInfo.Id);

// 2. In OnAllocateResult():
TTxId txId = TTxId(AllocateResult->Get()->TxIds.front());
Send(Self->SelfId(), BackupPropose(Self, txId, exportInfo, itemIdx));

// 3. In OnModifyResult():
if (record.GetStatus() == NKikimrScheme::StatusAccepted) {
    exportInfo->Items.at(itemIdx).WaitTxId = txId;
    SubscribeTx(txId);
}
```

### **Required Incremental Restore Pattern (TARGET)**
```cpp
// 1. Transaction allocation  
Send(Self->TxAllocatorClient, new TEvTxAllocatorClient::TEvAllocate(), 0, operationId);

// 2. In OnAllocateResult():
TTxId txId = TTxId(AllocateResult->Get()->TxIds.front());
Send(Self->SelfId(), IncrementalRestorePropose(Self, txId, sourcePathId, destPathId));

// 3. In OnModifyResult():
if (record.GetStatus() == NKikimrScheme::StatusAccepted) {
    // Track transaction and subscribe to completion
    SubscribeTx(txId);
}
```

### **Why This Pattern Works**
1. **Proper API Usage**: `ITransaction*` objects go to `Execute()`, not operation objects
2. **SchemeShard Integration**: Operations created by SchemeShard infrastructure, not manually
3. **Transaction Coordination**: `context.OnComplete.BindMsgToPipe()` ensures proper plan steps
4. **Proven Pattern**: Export system uses this successfully for similar backup operations

---

## 🎯 **IMMEDIATE ACTION ITEMS**

### **Priority 1: Create Propose Infrastructure** ⚠️ **URGENT**
1. **Create IncrementalRestorePropose function** following export system pattern
2. **Update transaction structure** with proper restore operation fields  
3. **Test propose creation** and event structure validation

### **Priority 2: Fix TTxProgress Transaction Pattern** ⚠️ **HIGH**
1. **Implement transaction lifecycle methods** (AllocateTxId, OnAllocateResult, etc.)
2. **Remove direct operation creation** from scan logic
3. **Add proper state tracking** for transaction progression

### **Priority 3: Integration Testing** 📋 **MEDIUM**
1. **Build and compile** with new transaction pattern
2. **Verify SchemeShard processing** of propose messages
3. **Test DataShard execution** and data transfer validation

### **Expected Timeline**
- **Phase 9**: 1-2 days (create propose infrastructure)
- **Phase 10**: 1 day (fix scan logic integration)  
- **Phase 11**: 1 day (testing and validation)
- **Total**: 3-4 days for complete fix

### **Success Criteria**
- ✅ **Compilation**: No API mismatch errors
- ✅ **Transaction Flow**: Proper SchemeShard operation creation and lifecycle
- ✅ **Data Transfer**: Incremental backup data applied correctly (value 20→2000)
- ✅ **Multiple Backups**: Sequential operations work without conflicts

---

## 📁 **KEY FILES AND REFERENCES**

### **Reference Implementation (Export System)**
- **`schemeshard_export__create.cpp`** - Complete transaction lifecycle pattern
- **`schemeshard_export_flow_proposals.cpp`** - Propose function implementations
- **`TTxProgress`** class structure and methods (AllocateTxId, OnAllocateResult, OnModifyResult)

### **Target Files for Implementation**
- **`schemeshard_incremental_restore_scan.cpp`** - Main scan logic requiring transaction pattern
- **`schemeshard_incremental_restore_flow_proposals.cpp`** - New file for propose functions
- **`schemeshard__operation_create_restore_incremental_backup.cpp`** - Existing operation (target of proposals)

### **Key APIs and Methods**
- **`Send(Self->TxAllocatorClient, new TEvTxAllocatorClient::TEvAllocate())`** - Transaction allocation
- **`Send(Self->SelfId(), IncrementalRestorePropose(...))`** - Send propose to SchemeShard
- **`TEvModifySchemeTransaction`** - Core propose message type
- **`TEvModifySchemeTransactionResult`** - Result handling
- **`SubscribeTx(txId)`** - Transaction completion subscription

### **Critical Protobuf Structures**
- **`MutableRestoreMultipleIncrementalBackups()`** - Correct transaction field
- **`AddSrc()` with `SetSrcPathId()` and `SetDstPathId()`** - Source/destination wiring
- **Transaction type and operation parameters** - Proper schema transaction setup

---

## 🔍 **DETAILED ANALYSIS SUMMARY**

### **What We Discovered**
1. **API Mismatch**: `Self->Execute()` expects `ITransaction*`, not `ISubOperation::TPtr`
2. **Correct Pattern**: Export system provides exact template for transaction-based operations
3. **Missing Infrastructure**: Need propose functions that create `TEvModifySchemeTransaction` events
4. **Transaction Lifecycle**: Complete pattern from allocation → propose → result → completion

### **What We Fixed Previously**
- ✅ **Event-based removal**: Eliminated direct DataShard event sending
- ✅ **Parameter discovery**: Source/destination path identification working
- ✅ **Operation structure**: Basic operation registration infrastructure
- ✅ **Build compatibility**: Resolved compilation errors from previous attempts

### **What Remains to Fix**
- ❌ **API Usage**: Replace operation creation with transaction pattern
- ❌ **Propose Infrastructure**: Create IncrementalRestorePropose function
- ❌ **Transaction Lifecycle**: Implement complete allocation → execution → completion flow
- ❌ **Integration Testing**: Verify end-to-end data transfer functionality

### **Success Metrics**
- **Build Success**: No compilation or API mismatch errors
- **Transaction Flow**: Proper SchemeShard operation creation via propose messages
- **DataShard Execution**: Execution units triggered with proper plan steps
- **Data Validation**: Incremental backup data applied (value change 20→2000)

**CONFIDENCE LEVEL**: High - Export system provides proven working pattern that directly applies to incremental restore with minimal adaptation required.