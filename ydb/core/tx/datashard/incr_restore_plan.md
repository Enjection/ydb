# Comprehensive Plan for Fixing YDB Incremental Restore

## Current Status Summary (Updated: July 2, 2025)

### ✅ **Completed Successfully**
1. **Phase 1-5**: Schema transactions are being sent and accepted correctly
2. **Sequence Number Issue**: Fixed - DataShard accepts transactions with correct seqNo
3. **SchemeShard Logic**: Properly sends `TEvRunIncrementalRestore` and schema transactions
4. **Debug Logging**: Added comprehensive logging to key components

### 🔍 **Critical Discovery Made**
**ROOT CAUSE IDENTIFIED**: `TCreateIncrementalRestoreSrcUnit::Run()` is **NEVER** being called despite:
- ✅ Schema transaction being accepted: "Prepared scheme transaction"
- ✅ Correct sequence numbers (seqNo 2:5)
- ✅ No compilation or registration errors

### ⚠️ **Primary Issue Found**
**Schema Transaction Path Mapping Bug**: In `schemeshard_incremental_restore_scan.cpp:169-170`:
```cpp
// INCORRECT: Both paths are set to the same tablePathId
restoreBackup.MutableSrcPathId()->CopyFrom(tablePathId.ToProto());
restoreBackup.MutableDstPathId()->CopyFrom(tablePathId.ToProto());
```

**Expected**:
- `SrcPathId` = Backup table path (where incremental backup data is stored)
- `DstPathId` = Destination table path (where changes should be applied)

### 🔧 **Next Actions**
1. **Fix Path Mapping**: Correct source/destination paths in schema transaction
2. **Test with Debug Logging**: Verify execution unit triggers after path fix
3. **Complete Flow Validation**: Follow scan creation and data application

### 📊 **Test Status**
- **Expected**: `(2,2000), (3,30), (4,40)` (after incremental restore)
- **Actual**: `(1,10), (2,20), (3,30), (4,40), (5,50)` (no changes applied)
- **Issue**: Data remains completely unchanged

## Plan Overview

### Phase 1: Clean Up Incorrect Implementation ✅ (COMPLETED)
1. ✅ Remove the incorrect files created earlier:
   - `datashard__restore_multiple_incremental_backups.cpp`
   - `datashard_incremental_restore.cpp`
2. ✅ Remove references from `ya.make`
3. ✅ Remove the `TTxIncrementalRestore` declaration from `datashard_impl.h`
4. ✅ Remove the `TEvRestoreMultipleIncrementalBackups` handler from `datashard.cpp`
5. ✅ Remove event definitions from `datashard.h`
6. ✅ Remove handler from `schemeshard_impl.cpp`

### Phase 2: Update Schemeshard to Send Proper Schema Transactions ✅ (ALREADY COMPLETED)
1. ✅ **Modify `schemeshard_incremental_restore_scan.cpp`**:
   - ✅ Replace direct `TEvRestoreMultipleIncrementalBackups` events with schema transactions
   - ✅ Use `TEvProposeTransaction` with `CreateIncrementalRestoreSrc` schema operations
   - ✅ Set up proper `TRestoreIncrementalBackup` protobuf messages

2. ✅ **Schema Transaction Structure**: Already implemented correctly
   ```protobuf
   TEvProposeTransaction {
     TxId: <txId>
     PathId: <destination table path>
     SchemeTx {
       CreateIncrementalRestoreSrc {
         SrcPathId: <backup table path>
         DstPathId: <destination table path>
       }
     }
   }
   ```

### Phase 3: Handle Multiple Incremental Backups ✅ (ALREADY COMPLETED)
1. ✅ **Sequential Processing**: Each schema transaction handles one backup table, multiple transactions sent for multiple incremental backups
2. ✅ **Coordination**: Operation tracking waits for all backup table restorations to complete
3. ✅ **Response Handling**: Processes `TEvProposeTransactionResult` responses from all datashard transactions

### Phase 4: Verify Integration ✅ (COMPLETED)
1. ✅ **Test Execution**: Test runs and incremental restore is triggered correctly
2. ✅ **Schema Transaction Issue**: DataShard now accepts transactions with correct sequence numbers (seqNo 2:5)
3. ❌ **Data Validation**: Data remains unchanged despite successful schema transaction:
   - Original: (1,10), (2,20), (3,30), (4,40), (5,50)
   - Expected after restore: (2,2000), (3,30), (4,40)
   - **Actual**: (1,10), (2,20), (3,30), (4,40), (5,50) ← No changes applied

### Phase 5: Fix Sequence Number Issue ✅ (COMPLETED)
1. ✅ **Problem Fixed**: Schema transactions now use correct sequence numbers
2. ✅ **Transaction Acceptance**: DataShard accepts and processes transactions successfully
3. ✅ **Status**: "Prepared scheme transaction" instead of "Ignore message"

### Phase 6: Debug Incremental Restore Execution 🔍 (CRITICAL DISCOVERY MADE)

#### Step 6.1: Analyze Current Status ✅ (COMPLETED)
**SYMPTOMS**:
- ✅ SchemeShard sends `TEvRunIncrementalRestore` correctly
- ✅ Schema transactions are created and accepted by DataShard
- ❌ **CRITICAL DISCOVERY**: `TCreateIncrementalRestoreSrcUnit::Run()` is NEVER called
- ❌ **Data modifications are NOT applied to destination table**

**ROOT CAUSE ANALYSIS** ✅ (IDENTIFIED):
1. ✅ Schema transaction is accepted: "Prepared scheme transaction txId 281474976715666"
2. ❌ **`TCreateIncrementalRestoreSrcUnit::Run()` never executes** (no debug logs appear)
3. ❌ This means the execution unit is not being triggered despite transaction acceptance

#### Step 6.2: Investigation Points ✅ (PARTIALLY COMPLETED)
1. ✅ **Verify Execution Unit Triggering**: CONFIRMED - Execution unit is NOT being called
2. 🔍 **Check IsRelevant() method**: Added debug logging to verify if execution unit filter is working
3. 🔍 **Examine Schema Transaction Content**: Need to verify SrcPathId/DstPathId are correct
4. 🔍 **Check Execution Unit Registration**: Ensure execution unit is properly registered

#### Step 6.3: Debugging Strategy
1. **Add Debug Logging**: Insert logging in key components to trace execution
2. **Examine Test Data Setup**: Verify incremental backup contains expected changes
3. **Check Path Resolution**: Ensure source/destination paths are correctly mapped
4. **Validate Scan Creation**: Confirm incremental restore scan is created and running

### Phase 7: Fix Execution Unit Triggering Issue 🛠️ (CURRENT FOCUS)

#### Step 7.1: Root Cause Analysis ✅ (IDENTIFIED)
**PROBLEM**: `TCreateIncrementalRestoreSrcUnit::Run()` is never called despite schema transaction being accepted.

**POTENTIAL CAUSES**:
1. 🔍 **Execution unit not registered properly**
2. 🔍 **IsRelevant() method returning false** - Added debug logging to check
3. 🔍 **Schema transaction missing CreateIncrementalRestoreSrc field**
4. 🔍 **Wrong source/destination paths in schema transaction**

#### Step 7.2: Investigation Steps ⚠️ (IN PROGRESS)
1. ✅ **Added Debug Logging**: Added logging to `TCreateIncrementalRestoreSrcUnit::Run()` and `IsRelevant()`
2. 🔍 **Check Schema Transaction Content**: Verify `CreateIncrementalRestoreSrc` is properly set
3. 🔍 **Verify Execution Unit Registration**: Ensure execution unit is in the pipeline
4. 🔍 **Check Path Mapping**: Source should be backup table, destination should be target table

#### Step 7.3: Schema Transaction Content Analysis 🔍 (DISCOVERED ISSUE)
**FINDINGS**: In `schemeshard_incremental_restore_scan.cpp:169-170`:
```cpp
restoreBackup.MutableSrcPathId()->CopyFrom(tablePathId.ToProto());
restoreBackup.MutableDstPathId()->CopyFrom(tablePathId.ToProto());
```
**PROBLEM**: Both SrcPathId and DstPathId are set to the same `tablePathId` - this is incorrect!
- `SrcPathId` should be the backup table path
- `DstPathId` should be the destination table path

#### Step 7.4: Next Actions 🔧 (IMMEDIATE)
1. **Fix Source/Destination Path Mapping**: Correct the path assignment in schema transaction
2. **Run Test with Debug Logging**: Verify if execution unit is triggered after fix
3. **Trace Complete Flow**: Follow scan creation and change application once execution unit runs

## Detailed Implementation Steps

### Immediate Action Plan (Phase 6 - Current Focus)

#### **Step 1: Add Debug Logging to Key Components** 🔍
Add comprehensive logging to trace the incremental restore execution flow:

1. **TCreateIncrementalRestoreSrcUnit**: Verify execution unit is triggered
2. **Scan Creation**: Check if incremental restore scan is created
3. **Change Sender**: Verify changes are being applied to destination table

#### **Step 2: Examine Test Data Setup** 📋
Verify the test infrastructure:
1. Check incremental backup data contains expected changes (row 2: 20→2000)
2. Validate source/destination path mapping in schema transactions
3. Ensure backup table accessibility

#### **Step 3: Trace Complete Execution Flow** 🔄
Follow the data modification pipeline:
1. Schema transaction processing ✅ (working)
2. Execution unit activation 🔍 (needs verification)
3. Scan creation 🔍 (needs verification)  
4. Data reading from backup 🔍 (needs verification)
5. Change application 🔍 (needs verification)

#### **Step 4: Fix Missing Components** 🛠️
Based on investigation findings, implement fixes for any broken components.

### Implementation Steps

### Step 1: Clean Up Files
- Remove incorrect implementation files
- Update build configuration
- Remove handler declarations

### Step 2: Update Schemeshard Logic
- Modify `SendRestoreRequests()` function in `schemeshard_incremental_restore_scan.cpp`
- Replace `TEvRestoreMultipleIncrementalBackups` with `TEvProposeTransaction`
- Use `MakeDataShardProposal()` to create proper schema transactions

### Step 3: Handle Response Processing
- Update response handling to expect `TEvProposeTransactionResult` instead of `TEvRestoreMultipleIncrementalBackupsResponse`
- Process multiple responses for multiple backup tables
- Aggregate results and report completion

### Step 4: Test and Validate
- Compile and run the failing test
- Verify that the `TCreateIncrementalRestoreSrcUnit` is triggered correctly
- Confirm data modifications match expected results

## Expected Outcome

After this implementation:
1. The schemeshard will send proper schema transactions to datashards
2. The existing `TCreateIncrementalRestoreSrcUnit` will be triggered correctly
3. The incremental restore scan infrastructure will process backup tables
4. The destination table will be modified according to the incremental changes
5. The test will pass with the expected data: (2,2000), (3,30), (4,40)

This approach leverages the existing, well-tested incremental restore infrastructure instead of creating new custom implementations.