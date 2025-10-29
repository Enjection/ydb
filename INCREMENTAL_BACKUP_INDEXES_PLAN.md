# Implementation Plan: Incremental Backup for Indexes

## Overview
This document outlines the implementation plan for supporting incremental backups of indexes in backup collections.

## Key Principles

1. **Indexes are simple tables** - We can reuse existing CDC stream operations for tables
2. **Lazy directory creation** - The `__ydb_backup_meta/indexes/...` directory structure is created only when an incremental backup is actually performed, not at backup collection creation
3. **Parallel CDC streams** - Launch separate CDC streams for each index in parallel with the main table
4. **Global sync indexes only** - This implementation covers only global sync indexes (`EIndexTypeGlobal`)
5. **No schema changes** - We do not handle index additions/removals/alterations between backups
6. **No restore yet** - This document covers only backup; restore is out of scope
7. **No compatibility** - No backward compatibility concerns for now

## Storage Structure

### Directory Layout

**Source side (main database):**
```
/Root/db1/
└── table1/                          # Main table
    ├── {timestamp}_continuousBackupImpl/  # CDC stream for main table
    ├── index1/                      # Index
    │   └── indexImplTable/          # Index implementation table
    │       └── {timestamp}_continuousBackupImpl/  # CDC stream for index1
    └── index2/                      # Index
        └── indexImplTable/          # Index implementation table
            └── {timestamp}_continuousBackupImpl/  # CDC stream for index2
```

**Backup side (backup collection):**
```
/my_backup_collection/
├── 2024-01-01_inc/                  # Incremental backup instance
│   ├── table1/                       # Backup table for main table data
│   └── __ydb_backup_meta/            # Service directory (created on demand)
│       └── indexes/
│           └── /Root/db1/table1/     # Full table path as directory
│               ├── index1/           # Backup table for index1 incremental data
│               └── index2/           # Backup table for index2 incremental data
```

**Key points:**
- CDC streams are created on **source** tables/indexes (in main database)
- Backup tables store the incremental data (in backup collection)
- For main table: CDC at `/Root/db1/table1/{timestamp}_continuousBackupImpl/`
- For index: CDC at `/Root/db1/table1/index1/indexImplTable/{timestamp}_continuousBackupImpl/`
- Service directory `__ydb_backup_meta` is created **inside** the incremental backup directory only when first incremental backup is performed

## Implementation Phases

### Phase 1: Understanding Current Architecture ✓ COMPLETED

#### 1.1 Current State Analysis
**Key Findings:**

1. **Index Structure** (verified in code):
   - Index path: `/table/index_name/` (PathType: `EPathTypeTableIndex`)
   - Implementation table: `/table/index_name/<impl_table>` (PathType: `EPathTypeTable`)
   - Each index has exactly ONE child - the implementation table
   - Index info stored in `TSchemeShard::Indexes` map (`TPathId` → `TTableIndexInfo::TPtr`)

2. **Current Restriction** (NOT RELEVANT):
   - File: `/ydb/core/tx/schemeshard/schemeshard_path.cpp:1053`
   - Method: `TPath::TChecker::CanBackupTable()`
   - **Note: This check is NOT used for incremental backups, only for old-style backups**
   - We can proceed without modifying this

3. **CDC Stream for Incremental Backup**:
   - Stream name suffix: `_continuousBackupImpl`
   - Created via `AlterContinuousBackup` operation
   - Uses `NCdc::DoCreateStreamImpl()` and `NCdc::DoCreatePqPart()`
   - Mode: `ECdcStreamModeUpdate`, Format: `ECdcStreamFormatProto`

4. **Backup Operations Flow**:
   - Full backup: `schemeshard__operation_backup_backup_collection.cpp`
   - Incremental: `schemeshard__operation_backup_incremental_backup_collection.cpp`
   - Uses `AlterContinuousBackup` to rotate CDC streams and create backup tables

**Key understanding:** Since indexes are implemented as hidden tables, we can treat them exactly like tables for CDC purposes.

---

### Phase 2: Backup Collection Operations

#### 2.1 Modify Full Backup Operation ✓ COMPLETED
**File to modify:**
- `/ydb/core/tx/schemeshard/schemeshard__operation_backup_backup_collection.cpp`

**Changes in `CreateBackupBackupCollection()` function:**
```cpp
// After processing main table in the loop
for (const auto& item : bc->Description.GetExplicitEntryList().GetEntries()) {
    const auto sPath = TPath::Resolve(item.GetPath(), context.SS);
    auto table = context.SS->Tables.at(sPath.Base()->PathId);
    
    // ... existing table backup code ...
    
    // NEW: Process indexes if incremental backup is enabled
    if (incrBackupEnabled && table->Indexes.size() > 0) {
        for (const auto& [indexPathId, indexInfo] : table->Indexes) {
            auto indexPath = TPath::Init(indexPathId, context.SS);
            
            // ONLY handle global sync indexes
            if (indexInfo->Type != NKikimrSchemeOp::EIndexTypeGlobal) {
                continue;
            }
            
            // Index has exactly one child - the implementation table
            Y_ABORT_UNLESS(indexPath.Base()->GetChildren().size() == 1);
            auto [implTableName, implTablePathId] = *indexPath.Base()->GetChildren().begin();
            
            auto indexTablePath = indexPath.Child(implTableName);
            
            // Create CDC stream ON THE SOURCE index implementation table
            NKikimrSchemeOp::TCreateCdcStream createCdcStreamOp;
            createCdcStreamOp.SetTableName(indexTablePath.PathString());
            auto& streamDescription = *createCdcStreamOp.MutableStreamDescription();
            streamDescription.SetName(streamName);  // Same name pattern as main table stream
            streamDescription.SetMode(NKikimrSchemeOp::ECdcStreamModeUpdate);
            streamDescription.SetFormat(NKikimrSchemeOp::ECdcStreamFormatProto);
            
            // Create CDC stream impl on source index impl table
            NCdc::DoCreateStreamImpl(result, createCdcStreamOp, opId, indexTablePath, false, false);
            
            // Create PQ part for the index CDC stream
            auto indexTable = context.SS->Tables.at(implTablePathId);
            TVector<TString> boundaries;
            const auto& partitions = indexTable->GetPartitions();
            boundaries.reserve(partitions.size() - 1);
            for (ui32 i = 0; i < partitions.size(); ++i) {
                const auto& partition = partitions.at(i);
                if (i != partitions.size() - 1) {
                    boundaries.push_back(partition.EndOfRange);
                }
            }
            
            auto streamPath = indexTablePath.Child(streamName);
            NCdc::DoCreatePqPart(result, createCdcStreamOp, opId, streamPath, streamName, indexTable, boundaries, false);
        }
    }
}
```

**What this does:**
- Creates CDC stream on the **source** index impl table: `/Root/db1/table1/index1/indexImplTable/{timestamp}_continuousBackupImpl/`
- The CDC stream will capture changes to the index
- Backup data destination is NOT specified here (full backup doesn't use it)

#### 2.2 Modify Incremental Backup Operation  
**File to modify:**
- `/ydb/core/tx/schemeshard/schemeshard__operation_backup_incremental_backup_collection.cpp`

**Changes in `CreateBackupIncrementalBackupCollection()` function:**
```cpp
// After creating AlterContinuousBackup for main table
TVector<TPathId> streams;
for (const auto& item : bc->Description.GetExplicitEntryList().GetEntries()) {
    const auto tablePath = TPath::Resolve(item.GetPath(), context.SS);
    auto table = context.SS->Tables.at(tablePath.Base()->PathId);
    
    // Split path to get relative path from database root
    std::pair<TString, TString> paths;
    TString err;
    if (!TrySplitPathByDb(item.GetPath(), bcPath.GetDomainPathString(), paths, err)) {
        result = {CreateReject(opId, NKikimrScheme::StatusInvalidParameter, err)};
        return {};
    }
    auto& relativeItemPath = paths.second;
    
    // ... existing AlterContinuousBackup for main table ...
    
    // NEW: Process indexes
    if (table->Indexes.size() > 0) {
        for (const auto& [indexPathId, indexInfo] : table->Indexes) {
            auto indexPath = TPath::Init(indexPathId, context.SS);
            TString indexName = indexPath.LeafName();
            
            // ONLY handle global sync indexes
            if (indexInfo->Type != NKikimrSchemeOp::EIndexTypeGlobal) {
                continue;
            }
            
            // Get index implementation table (first and only child)
            Y_ABORT_UNLESS(indexPath.Base()->GetChildren().size() == 1);
            auto [implTableName, implTablePathId] = *indexPath.Base()->GetChildren().begin();
            
            // Build full path to index impl table (SOURCE table in main database)
            TString indexImplTablePath = JoinPath({item.GetPath(), indexName, implTableName});
            
            // Create AlterContinuousBackup for index impl table
            NKikimrSchemeOp::TModifyScheme modifyScheme;
            modifyScheme.SetWorkingDir(tx.GetWorkingDir());
            modifyScheme.SetOperationType(NKikimrSchemeOp::ESchemeOpAlterContinuousBackup);
            modifyScheme.SetInternal(true);
            
            auto& cb = *modifyScheme.MutableAlterContinuousBackup();
            cb.SetTableName(indexImplTablePath);  // Source: /Root/db1/table1/index1/indexImplTable
            
            auto& ib = *cb.MutableTakeIncrementalBackup();
            // Destination: where to create the backup table that will receive incremental data
            // Path: {backup_collection}/{timestamp}_inc/__ydb_backup_meta/indexes/{table_path}/{index_name}
            TString dstPath = JoinPath({
                tx.GetBackupIncrementalBackupCollection().GetName(),
                tx.GetBackupIncrementalBackupCollection().GetTargetDir(),
                "__ydb_backup_meta",
                "indexes",
                relativeItemPath,  // Relative table path (e.g., "table1")
                indexName          // Index name (e.g., "index1")
            });
            ib.SetDstPath(dstPath);
            
            TPathId stream;
            if (!CreateAlterContinuousBackup(opId, modifyScheme, context, result, stream)) {
                return result;
            }
            streams.push_back(stream);
        }
    }
}
```

**Key operations:**
1. Iterate through table's indexes (via `table->Indexes` map)
2. Get index implementation table (first and only child of index path)
3. Create `AlterContinuousBackup` operation for source index impl table at `/Root/db1/table1/index1/indexImplTable`
4. Specify destination backup table at `{backup_collection}/{timestamp}_inc/__ydb_backup_meta/indexes/{table_path}/{index_name}`
5. `CreateAlterContinuousBackup` will:
   - Rotate CDC stream on source index impl table: `/Root/db1/table1/index1/indexImplTable/{new_timestamp}_continuousBackupImpl/`
   - Create backup table at destination in backup collection
   - Configure CDC stream to send data to backup table

---

### Phase 3: Testing Strategy (PARTIALLY COMPLETED)

#### 3.1 Unit Tests - Phase 2.1 Tests ✓ COMPLETED
**Files modified:**
- `/ydb/core/tx/schemeshard/ut_backup_collection/ut_backup_collection.cpp` (added 6 tests)

**Test scenarios for Phase 2.1 (Full Backup CDC Stream Creation):**

1. **SingleTableWithGlobalSyncIndex** ✓:
   - Table with one global sync covering index
   - Full backup
   - **VERIFIES**: CDC stream on index impl table
   - **VERIFIES**: CDC stream names match pattern

2. **SingleTableWithMultipleGlobalSyncIndexes** ✓:
   - Table with 2 global sync indexes (covering and non-covering)
   - Full backup
   - **VERIFIES**: All index CDC streams created
   - **VERIFIES**: All streams use same timestamp

3. **MultipleTablesWithIndexes** ✓:
   - 2 tables, each with 1 global sync index
   - Full backup
   - **VERIFIES**: Independent CDC streams per index
   - **VERIFIES**: Correct isolation between tables

4. **TableWithMixedIndexTypes** ✓:
   - Table with global sync + async indexes
   - **VERIFIES**: Only global sync indexes get CDC streams
   - **VERIFIES**: Async indexes are skipped

5. **IncrementalBackupWithIndexes** ✓:
   - Table with 1 global sync index
   - Full backup + incremental backup
   - **VERIFIES**: CDC streams created on main table and index ✓
   - **VERIFIES**: Main table backup table created ✓
   - **NOT VERIFIED**: Index backup table (Phase 2.2 - has TODO comment for later)

6. **OmitIndexesFlag** ✓:
   - Table with global sync index
   - IncrementalBackupConfig with `OmitIndexes: true`
   - **VERIFIES**: Main table gets CDC stream
   - **VERIFIES**: Index does NOT get CDC stream (flag respected)

**What's tested (Phase 2.1 only):**
- ✅ CDC streams created on correct source tables
- ✅ CDC streams on index implementation tables
- ✅ Only global sync indexes get CDC streams
- ✅ CDC stream naming follows pattern
- ✅ Multiple indexes handled correctly
- ✅ `OmitIndexes` flag is respected (no CDC on indexes when true)
- ✅ Incremental backup disabled = no CDC on indexes

**What's NOT tested yet (requires Phase 2.2):**
- ❌ Service directory `__ydb_backup_meta/indexes` creation
- ❌ Backup tables created at `__ydb_backup_meta/indexes/{table}/{index}`
- ❌ Incremental backup operation for indexes

---

## Implementation Priority Order

1. **Phase 2.1**: Modify full backup operation ✅ COMPLETED
   - CDC stream creation for indexes implemented
   - Code passes linting

2. **Phase 2.2**: Modify incremental backup operation ⏳ PENDING
   - Backup table creation for indexes
   - Service directory structure creation

3. **Phase 3**: Testing ⚠️ PARTIALLY COMPLETED
   - Phase 2.1 tests completed (6 tests should pass)
   - Phase 2.2 test coverage added as TODO comments in test code

**Out of scope:**
- Restore operations
- Index schema changes
- Non-global-sync index types
- Compatibility with old backups
- Path validation (using existing validation)

**Simplifications:**
- No new CDC code needed (reuse existing `CreateAlterContinuousBackup`)
- Only global sync indexes
- Order of CDC creation doesn't matter (all enabled at the end)

**Error handling:**
- If any index CDC creation fails, entire backup operation fails

**Total estimated time:** 1-2 weeks

---

## Key Technical Decisions

1. **Reuse existing CDC stream operations** - Indexes are tables, no need for special CDC logic
2. **Lazy directory creation** - `__ydb_backup_meta` structure created only when needed
3. **Path encoding** - Use full table path as directory name to avoid collisions
4. **Stream naming** - Use **SAME** stream name as main table (`{timestamp}_continuousBackupImpl`)
5. **No protobuf changes** - Reuse existing transaction types
6. **Index iteration** - Use `TTableInfo::Indexes` map to enumerate indexes
7. **Implementation table access** - Get via `indexPath.Base()->GetChildren()` (exactly 1 child)

---

## Files to Modify (Concrete List)

### Phase 2: Backup Operations (ONLY THESE TWO FILES!)
1. **`/ydb/core/tx/schemeshard/schemeshard__operation_backup_backup_collection.cpp`**
   - Function: `CreateBackupBackupCollection()`
   - **Action**: Add index CDC stream creation after main table processing
   - **Filter**: Only process `EIndexTypeGlobal` indexes
   - **Error handling**: If index CDC creation fails, entire operation fails

2. **`/ydb/core/tx/schemeshard/schemeshard__operation_backup_incremental_backup_collection.cpp`**
   - Function: `CreateBackupIncrementalBackupCollection()`
   - **Action**: Add index AlterContinuousBackup operations
   - **Filter**: Only process `EIndexTypeGlobal` indexes
   - **Error handling**: If index CDC creation fails, entire operation fails

### Referenced (No Changes Needed)
- `/ydb/core/tx/schemeshard/schemeshard__operation_alter_continuous_backup.cpp` - Reuse as-is
- `/ydb/core/tx/schemeshard/schemeshard__backup_collection_common.h` - Reference for path helpers
- `/ydb/core/tx/schemeshard/schemeshard_info_types.h` - Reference for `TTableInfo::Indexes` and `TTableIndexInfo::Type`

---

## Critical Code Patterns Discovered

### How to Iterate Through Table Indexes

```cpp
// Get table info
auto table = context.SS->Tables.at(tablePath.Base()->PathId);

// Iterate through indexes
for (const auto& [indexPathId, indexInfo] : table->Indexes) {
    // indexPathId: TPathId of the index path
    // indexInfo: TTableIndexInfo::TPtr with index metadata
    
    auto indexPath = TPath::Init(indexPathId, context.SS);
    TString indexName = indexPath.LeafName();
    
    // Get index implementation table (exactly 1 child)
    Y_ABORT_UNLESS(indexPath.Base()->GetChildren().size() == 1);
    auto [implTableName, implTablePathId] = *indexPath.Base()->GetChildren().begin();
    
    // Now you can work with the implementation table
    auto indexTablePath = indexPath.Child(implTableName);
    auto indexTable = context.SS->Tables.at(implTablePathId);
}
```

### How CDC Streams Are Created in Backup Collections

```cpp
// Full backup - create initial CDC stream ON SOURCE TABLE
// Example: Creates /Root/db1/table1/{timestamp}_continuousBackupImpl/
// For indexes: Creates /Root/db1/table1/index1/indexImplTable/{timestamp}_continuousBackupImpl/
NKikimrSchemeOp::TCreateCdcStream createCdcStreamOp;
createCdcStreamOp.SetTableName(tablePathStr);  // Source table path
auto& streamDescription = *createCdcStreamOp.MutableStreamDescription();
streamDescription.SetName(streamName);
streamDescription.SetMode(NKikimrSchemeOp::ECdcStreamModeUpdate);
streamDescription.SetFormat(NKikimrSchemeOp::ECdcStreamFormatProto);

NCdc::DoCreateStreamImpl(result, createCdcStreamOp, opId, tablePath, false, false);
NCdc::DoCreatePqPart(result, createCdcStreamOp, opId, streamPath, streamName, table, boundaries, false);
```

```cpp
// Incremental backup - rotate CDC stream on SOURCE and create backup table at DESTINATION
// Source CDC: /Root/db1/table1/{new_timestamp}_continuousBackupImpl/
// For indexes: /Root/db1/table1/index1/indexImplTable/{new_timestamp}_continuousBackupImpl/
// Destination backup table: /backup_collection/123_inc/table1/ (or __ydb_backup_meta/indexes/...)
NKikimrSchemeOp::TModifyScheme modifyScheme;
modifyScheme.SetOperationType(NKikimrSchemeOp::ESchemeOpAlterContinuousBackup);
auto& cb = *modifyScheme.MutableAlterContinuousBackup();
cb.SetTableName(sourceTablePath);  // SOURCE table (where CDC lives)
auto& ib = *cb.MutableTakeIncrementalBackup();
ib.SetDstPath(backupDestinationPath);  // DESTINATION backup table

TPathId stream;
CreateAlterContinuousBackup(opId, modifyScheme, context, result, stream);
```

### Path Construction Helpers

```cpp
// Join paths
TString indexBackupPath = JoinPath({
    backupCollectionName,
    incrementalDirName,  // e.g., "2024-01-01_incremental"
    "__ydb_backup_meta",
    "indexes",
    relativeTablePath,
    indexName
});

// Split path by database
std::pair<TString, TString> paths;
TString err;
if (!TrySplitPathByDb(absolutePath, databasePath, paths, err)) {
    // Error handling
}
auto& relativePath = paths.second;
```

---

## Success Criteria

**Phase 2.1 (Full Backup) - COMPLETED:**
- [x] Full backups create CDC streams on global sync index impl tables
- [x] CDC streams created for each global sync index in parallel with main table
- [x] Non-global-sync indexes are skipped
- [x] Unit tests written for CDC stream creation
- [x] All code passes linting
- [ ] Tests pass when executed (requires build)

**Phase 2.2 (Incremental Backup) - NOT YET IMPLEMENTED:**
- [ ] Incremental backups create backup tables in `__ydb_backup_meta/indexes/{table_path}/{index_name}`
- [ ] Service directory created only when incremental backup is performed
- [ ] Tests for incremental backup functionality (1 test written but will fail until implemented)
- [ ] Performance acceptable with multiple indexes

---

## Scope Clarifications ✓

1. **Index Types**: ✓ ONLY GLOBAL SYNC INDEXES
   - Support: `EIndexTypeGlobal` only
   - Skip: All other types (async, vector, fulltext, etc.)

2. **Restore Strategy**: OUT OF SCOPE
   - Not covered in this document
   - Will be addressed separately

3. **Directory Structure**: ✓ CLARIFIED
   - Source CDC path: `/Root/db1/table1/index1/indexImplTable/{timestamp}_continuousBackupImpl/`
   - Backup table path: `{backup_collection}/{timestamp}_inc/__ydb_backup_meta/indexes/{relative_table_path}/{index_name}`
   - CDC stream lives on SOURCE index impl table, NOT on backup table
   - Use `TrySplitPathByDb()` to get relative path from database root

4. **Schema Changes**: NOT SUPPORTED
   - No handling of index additions/removals between backups
   - No handling of index alterations

5. **Compatibility**: NOT A CONCERN
   - No backward compatibility requirements for now
