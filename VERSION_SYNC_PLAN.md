# Version Synchronization Research and Fix Plan

## Problem Statement
After backup with CDC stream creation, querying the source table fails with:
```
schema version mismatch during metadata loading for: /Root/TableWithIndex/idx/indexImplTable expected 1 got 2
```

This happens on the SOURCE table after backup, not during restore.

## Root Cause Hypothesis
When CDC streams are created during backup:
1. Main table gets CDC → AlterVersion increments (1→2)
2. Index impl table gets CDC → AlterVersion increments (1→2)
3. **BUT** the index metadata in the main table still references version 1 for impl table
4. When querying, KQP expects version 1 (from index metadata) but sees version 2 (actual impl table version)

---

## Phase 1: Research - Understand Current Version State

### Step 1.1: Add Diagnostic Logging to Backup Operation

**File**: `ydb/core/tx/schemeshard/schemeshard__operation_backup_backup_collection.cpp`

**Location**: After line 180 (after all CDC streams are created)

**Add**:
```cpp
LOG_NOTICE_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
    "Backup CDC creation completed for table: " << tablePath.PathString()
    << ", MainTable AlterVersion: " << table->AlterVersion);

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
    
    auto* implTable = context.SS->Tables.FindPtr(implTablePathId);
    if (implTable) {
        LOG_NOTICE_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
            "IndexImplTable: " << implTableName
            << ", AlterVersion: " << (*implTable)->AlterVersion
            << ", PathId: " << implTablePathId);
    }
}
```

### Step 1.2: Add Test Diagnostics

**File**: `ydb/core/tx/datashard/datashard_ut_incremental_backup.cpp`

**Location**: In `SimpleBackupRestoreWithIndex` test, after backup operation (around line 566, before the query)

**Add**:
```cpp
// Add version diagnostics before query
{
    Cerr << "========== VERSION DIAGNOSTICS AFTER BACKUP ==========" << Endl;
    
    auto describeTable = Ls(runtime, edgeActor, "/Root/TableWithIndex");
    Cerr << "Main table version: " << describeTable->ResultSet.GetPathDescription().GetTable().GetVersion() << Endl;
    Cerr << "Main table path version: " << describeTable->ResultSet.GetPathDescription().GetSelf().GetPathVersion() << Endl;
    
    auto describeIndex = Ls(runtime, edgeActor, "/Root/TableWithIndex/idx");
    Cerr << "Index path version: " << describeIndex->ResultSet.GetPathDescription().GetSelf().GetPathVersion() << Endl;
    
    auto describeImplTable = Ls(runtime, edgeActor, "/Root/TableWithIndex/idx/indexImplTable");
    Cerr << "Index impl table version: " << describeImplTable->ResultSet.GetPathDescription().GetTable().GetVersion() << Endl;
    Cerr << "Index impl table path version: " << describeImplTable->ResultSet.GetPathDescription().GetSelf().GetPathVersion() << Endl;
    
    Cerr << "======================================================" << Endl;
}
```

### Step 1.3: Run Test with Enhanced Logging

```bash
cd /home/innokentii/ydbwork2/ydb
./ya make -tA ydb/core/tx/datashard/ut_incremental_backup --test-filter SimpleBackupRestoreWithIndex 2>&1 | tee /tmp/version_debug.log

# Extract version info from logs
grep -E "AlterVersion|VERSION DIAGNOSTICS|Index.*version" /tmp/version_debug.log
```

---

## Phase 2: Analysis - Understand Version Flow

### Questions to Answer from Logs:

1. **Initial state**: What are the versions of all three entities after table creation with index?
   - Main table version: ?
   - Index version: ?
   - Index impl table version: ?

2. **After CDC creation**: What are the versions after backup CDC stream creation?
   - Main table version: ?
   - Index version: ?
   - Index impl table version: ?

3. **Version tracking**: Where is the expected version stored?
   - Does the main table track the expected impl table version?
   - Does the index metadata have a version field?

---

## Phase 3: Find Existing Sync Mechanisms

### Code Search Tasks:

```bash
# Search for index version tracking fields
grep -r "IndexAlterVersion\|tableIndex.*AlterVersion" ydb/core/tx/schemeshard/ --include="*.h"

# Search for impl table version tracking
grep -r "ImplTableAlterVersion\|IndexImplTable.*Version" ydb/core/tx/schemeshard/ --include="*.h"

# Look for TTableIndexInfo structure
grep -r "struct.*TTableIndexInfo\|class.*TTableIndexInfo" ydb/core/tx/schemeshard/ --include="*.h" -A 20

# Look for schema version update mechanisms
grep -r "UpdateSchemaVersion\|SchemaVersion.*Update" ydb/core/tx/schemeshard/ --include="*.cpp"

# Look for how KQP resolves table metadata
grep -r "ResolveTables\|GetTableInfo" ydb/core/kqp/ --include="*.cpp" | grep -i version
```

### Files to Examine:

1. `ydb/core/tx/schemeshard/schemeshard_info_types.h` - Table and Index metadata structures
2. `ydb/core/tx/schemeshard/schemeshard__operation_create_indexed_table.cpp` - How indexes are created
3. `ydb/core/tx/schemeshard/schemeshard__operation_alter_table.cpp` - How versions are updated
4. `ydb/core/kqp/kqp_metadata_loader.cpp` - How KQP loads metadata and checks versions

---

## Phase 4: Identify the Fix Location

### Expected Findings:

Based on typical patterns, we expect to find:

1. **TTableIndexInfo structure** has a field tracking impl table version or state
2. **Main table's TableInfo** holds references to indexes and their expected versions
3. **KQP metadata loader** compares expected vs actual versions when loading
4. **Schema change notifications** are sent when versions change

### Likely Root Cause:

The fix will need to update the index metadata or main table metadata after CDC stream creation on impl tables.

---

## Phase 5: Implement Version Synchronization

### Option A: Update Index Metadata (Preferred)

If `TTableIndexInfo` has a version tracking field:

```cpp
// In schemeshard__operation_backup_backup_collection.cpp
// After CDC stream creation for index impl tables (after line 178)

if (incrBackupEnabled && !omitIndexes) {
    // Synchronize index metadata with new impl table versions
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
        
        auto indexPath = TPath::Init(childPathId, context.SS);
        Y_ABORT_UNLESS(indexPath.Base()->GetChildren().size() == 1);
        auto [implTableName, implTablePathId] = *indexPath.Base()->GetChildren().begin();
        
        auto* implTable = context.SS->Tables.FindPtr(implTablePathId);
        if (implTable) {
            // Update index metadata to reflect new impl table version
            // TODO: Find the correct field name from Phase 3 research
            // indexInfo->ImplTableAlterVersion = (*implTable)->AlterVersion;
            
            // Or: increment main table version to trigger metadata refresh
            table->AlterVersion++;
            
            LOG_NOTICE_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                "Synchronized index version: " << childName
                << ", impl table version: " << (*implTable)->AlterVersion);
        }
    }
}
```

### Option B: Increment Main Table Version

If no explicit sync mechanism exists, increment main table version:

```cpp
// After all CDC streams created
if (incrBackupEnabled && !omitIndexes && hasCreatedIndexImplCdc) {
    // Increment main table version to force KQP metadata refresh
    table->AlterVersion++;
    
    // Send schema change notification to datashards
    context.SS->NotifySchemaChange(tablePath.Base()->PathId, table->AlterVersion);
}
```

### Option C: Send Explicit Version Update

```cpp
// Send schema version update to datashards
for (auto& shard : table->GetPartitions()) {
    auto event = MakeHolder<TEvDataShard::TEvSchemaChanged>();
    event->Record.SetPathOwnerId(tablePath.Base()->PathId.OwnerId);
    event->Record.SetLocalPathId(tablePath.Base()->PathId.LocalPathId);
    event->Record.SetGeneration(table->AlterVersion);
    context.OnComplete.Send(shard.DatashardId, std::move(event));
}
```

---

## Phase 6: Verify the Fix

### Test Plan:

1. **Build with logging**:
   ```bash
   cd /home/innokentii/ydbwork2/ydb
   ./ya make -r ydb/core/tx/schemeshard ydb/core/tx/datashard/ut_incremental_backup
   ```

2. **Run test with verbose output**:
   ```bash
   ./ya make -tA ydb/core/tx/datashard/ut_incremental_backup --test-filter SimpleBackupRestoreWithIndex -v 2>&1 | tee /tmp/version_fix_test.log
   ```

3. **Verify**:
   - No "schema version mismatch" errors
   - All three entities show synchronized versions in logs
   - Query on source table works after backup
   - Backup and restore both succeed

4. **Run full test suite**:
   ```bash
   ./ya make -tA ydb/core/tx/datashard/ut_incremental_backup
   ```

---

## Success Criteria

- [ ] Logs show correct version values for table, index, and impl table
- [ ] No version mismatch errors when querying source table after backup
- [ ] SimpleBackupRestoreWithIndex test passes
- [ ] All incremental backup tests pass
- [ ] Incremental backup functionality works correctly with indexes
