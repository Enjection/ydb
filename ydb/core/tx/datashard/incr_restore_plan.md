# YDB Incremental Restore Fix - Updated Implementation Plan

## Final Target
- The incremental restore logic must apply all incremental backups in order for each table, not just the first, so that the restored table matches the expected state (including all value changes and deletions) after a full + multiple incrementals restore.
- The test `SimpleRestoreBackupCollection, WithIncremental` must pass, confirming that the restored table contains only the correct rows and values.

## ROOT CAUSE IDENTIFIED: SchemeShard Operation Handler Issue

### Current Status (as of July 3, 2025)
- ✅ **Root cause analysis completed**: The issue is in `TConfigurePartsAtTable::FillNotice()` in `schemeshard__operation_create_restore_incremental_backup.cpp`
- ✅ **Error pattern confirmed**: All test failures show `deletedMarkerColumnFound` violation in DataShard, indicating wrong source table references
- ✅ **Architecture verified**: SchemeShard correctly collects and sorts multiple incremental sources, but operation handler only processes the first one
- 🔄 **Fix pending**: Need to modify operation handler to process ALL sources instead of just `RestoreOp.GetSrcPathIds(0)`

## IMMEDIATE ACTION REQUIRED

### Fix SchemeShard Operation Handler
**File**: `/ydb/core/tx/schemeshard/schemeshard__operation_create_restore_incremental_backup.cpp`
**Method**: `TConfigurePartsAtTable::FillNotice()`
**Current Issue**: Only processes `RestoreOp.GetSrcPathIds(0)` (first source)
**Required Fix**: Iterate through ALL sources in `RestoreOp.GetSrcPathIds()` and create separate DataShard transactions

### Implementation Strategy
1. **Modify FillNotice() method**:
   - Replace single source processing with loop over all sources
   - Create separate transaction for each incremental backup
   - Maintain timestamp order for correct sequence

2. **Update ProgressState handling**:
   - Ensure multiple DataShard transactions are tracked correctly
   - Process results from all incremental backup operations

3. **Test the fix**:
   - Build and run `SimpleRestoreBackupCollection, WithIncremental` test
   - Verify all incremental backups are applied in correct order
   - Confirm final restored state matches expected values

## ANALYSIS COMPLETED - Architecture Overview (Confirmed Working)

### 1. Complete OnAllocateResult Implementation
- Enhance `OnAllocateResult` in `schemeshard_incremental_restore_scan.cpp` to send `IncrementalRestorePropose` with the correct source/destination context for each incremental restore operation.
- Retrieve context (source/destination paths) from operation tracking or context storage.
- Track transaction: `Self->TxIdToIncrementalRestore[txId] = operationId;`

### 2. Add Context Storage for Source/Destination Paths
- Extend operation tracking/context structures to store source backup path and destination table path during scan.
- Ensure this context is available in `OnAllocateResult` for propose creation.

### 3. Add Transaction Completion Notification
- Implement transaction completion notification handling in `schemeshard_impl.cpp`.
- Follow the export system's `TxIdToExport.contains(txId)` pattern for cleanup and next-step triggering.
- The next `TTxProgress` for the following incremental transfer should only be started when the previous transfer is fully finished (transaction completion notification received).

### 4. Refactor to Apply All Incremental Backups in Order
- In the incremental restore scan logic, collect all incremental backups for each table, sort them, and apply them one by one (not just the first).
- Ensure context and transaction flow is preserved for each incremental.
- Progress state (e.g., which incremental/table is next) must be persisted in SchemeShard's local database, so that the operation can be resumed from the last completed table in case of a restart or failure.

### 5. Build, Integration, and End-to-End Testing
- Build the project to verify that all API mismatches are resolved and the transaction pattern is correct.
- Add logging and test the transaction lifecycle (allocation → propose → result) to verify the flow is correct.
- Run integration and end-to-end tests to verify:
  - All incrementals are applied in order.
  - The test `SimpleRestoreBackupCollection, WithIncremental` passes (restored table matches expectations, including value changes and deletions).

---

## Key Findings and References
- The correct pattern is modeled after the export system (`schemeshard_export__create.cpp`, `schemeshard_export_flow_proposals.cpp`): allocate transaction, send propose, handle result, subscribe to completion.
- Use `IncrementalRestorePropose` to create a `TEvModifySchemeTransaction` event with the correct protobuf structure (`MutableRestoreMultipleIncrementalBackups`, `AddSrc()->SetSrcPathId()/SetDstPathId()`).
- All transaction state and context must be preserved through the lifecycle, as in the export system.
- The restore scan logic must iterate through all incremental backups for each table, not just the first, and apply them in order.
- Each incremental transfer must be fully completed before starting the next one.
- Progress state must be persisted in SchemeShard's local database to allow safe resumption after failures.
- Success is measured by passing the end-to-end test and matching the expected restored table state.

---

**Summary:**
- The core infrastructure is in place and compiling. The remaining work is to ensure all incremental backups are applied in order, context is preserved through the transaction lifecycle, and the transaction flow is fully completed and tested. The final target is a correct, fully incremental restore as validated by the test suite.