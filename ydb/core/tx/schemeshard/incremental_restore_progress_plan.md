# 📋 Incremental Restore Implementation Plan - REVISED

## 📊 Current Status Evaluation

### ✅ Completed from Original Plan:
1. **Proto syntax error** - Fixed in `counters_schemeshard.proto`
2. **Event definitions** - Added to `tx_datashard.proto`
3. **Event classes** - Added to `datashard.h`
4. **Handler registration** - Added to `datashard_impl.h`
5. **Basic DataShard handler** - Created `datashard_incremental_restore.cpp`
6. **Header file** - Created `datashard_incremental_restore.h`
7. **Build system update** - Added to `ya.make`
8. **SchemeShard handlers** - Added to `schemeshard_impl.cpp`
9. **Progress tracking** - Basic implementation in `schemeshard_incremental_restore_scan.cpp`

### ❌ Issues Found from Analysis:
1. **Include path** - Still using `.cpp` instead of `.h` in `datashard.cpp`
2. **Duplicate implementation** - Both `.cpp` and `.h` files have the same class
3. **Missing multi-step logic** - Current implementation doesn't handle multiple incremental backups properly
4. **No proper state machine** - Unlike build_index, doesn't continue to next incremental backup
5. **Missing integration** - Not properly connected to `MultiIncrementalRestore` operation

## � Build Index Pattern Analysis

The build_index pattern shows:
- **State Machine**: Progress through states (Allocating → Proposing → Waiting → Applying → Done)
- **Shard Tracking**: Maintains `InProgressShards`, `DoneShards`, `ToProcessShards`
- **Iterative Processing**: Processes shards in batches, moving to next batch when current is done
- **Progress Persistence**: Saves state to database for recovery

## 🏗️ Revised Architecture

```
User Request → RestoreBackupCollection → MultiIncrementalRestore
                                              ↓
                                      Multi-Step State Machine
                                              ↓
                                   Process Incremental Backup #1
                                              ↓
                                   Process Incremental Backup #2
                                              ↓
                                   Process Incremental Backup #N
                                              ↓
                                           Done
```

### Core Principle: **Sequential Processing of Incremental Backups**

Each incremental backup must be processed completely before moving to the next one, maintaining chronological order.

## 📝 Revised Step-by-Step Implementation Plan

### Step 1: Fix Immediate Issues 🚨 HIGH PRIORITY
- [ ] **File**: `ydb/core/tx/datashard/datashard.cpp`
- [ ] **Current**: `#include "datashard_incremental_restore.cpp"`
- [ ] **Fix**: Change to `#include "datashard_incremental_restore.h"`
- [ ] **Action**: Update include path

### Step 2: Remove Duplicate Implementation
- [ ] **File**: `ydb/core/tx/datashard/datashard_incremental_restore.h`
- [ ] **Action**: Delete the class implementation from header
- [ ] **Keep**: Only class declaration in header
- [ ] **Result**: Implementation stays only in `.cpp` file

### Step 3: Implement Multi-Step State Machine in SchemeShard
- [ ] **File**: `ydb/core/tx/schemeshard/schemeshard_incremental_restore_scan.cpp`
- [ ] **Action**: Update `TIncrementalRestoreContext` structure
- [ ] **Add**: Support for multiple incremental backups
- [ ] **Add**: Current incremental index tracking
- [ ] **Add**: State machine logic similar to build_index

```cpp
struct TIncrementalRestoreContext {
    // Multi-step incremental processing
    struct TIncrementalBackup {
        TPathId BackupPathId;
        TString BackupPath;
        ui64 Timestamp;
        bool Completed = false;
    };
    
    TVector<TIncrementalBackup> IncrementalBackups; // Sorted by timestamp
    ui32 CurrentIncrementalIdx = 0;
    
    bool IsCurrentIncrementalComplete() const;
    bool AllIncrementsProcessed() const;
    void MoveToNextIncremental();
};
```

### Step 4: Update Progress Transaction Logic
- [ ] **File**: `ydb/core/tx/schemeshard/schemeshard_incremental_restore_scan.cpp`
- [ ] **Action**: Update `TTxProgressIncrementalRestore` class
- [ ] **Add**: State handling for `Waiting` and `Applying` states
- [ ] **Add**: Logic to move to next incremental backup when current is complete
- [ ] **Add**: Method to start next incremental backup processing

### Step 5: Update DataShard Response Handler
- [ ] **File**: `ydb/core/tx/schemeshard/schemeshard_incremental_restore_scan.cpp`
- [ ] **Action**: Update response handler to track per-incremental progress
- [ ] **Add**: Logic to detect when current incremental is complete
- [ ] **Add**: Automatic progression to next incremental backup
- [ ] **Add**: Error handling and retry logic

### Step 6: Integration with MultiIncrementalRestore
- [ ] **File**: `ydb/core/tx/schemeshard/schemeshard__operation_restore_backup_collection.cpp`
- [ ] **Action**: Update `MultiIncrementalRestore::RunIncrementalRestore` method
- [ ] **Add**: Create context with all incremental backups upfront
- [ ] **Add**: Sort incremental backups by timestamp
- [ ] **Add**: Initialize state machine with first incremental backup

### Step 7: Simplify DataShard Handler
- [ ] **File**: `ydb/core/tx/datashard/datashard_incremental_restore.cpp`
- [ ] **Action**: Remove complex validation logic
- [ ] **Keep**: Simple acknowledgment logic only
- [ ] **Purpose**: DataShard just acknowledges, real work via change senders

### Step 8: Remove Over-engineered Code
- [ ] **File**: `ydb/core/tx/datashard/datashard_incremental_restore_request.cpp`
- [ ] **Action**: Delete this file (not needed)
- [ ] **Reason**: Over-engineering, not following build_index pattern

### Step 9: Build and Test
- [ ] **Action**: Compile DataShard module
- [ ] **Action**: Compile SchemeShard module
- [ ] **Fix**: Address compilation errors from refactoring

### Step 10: Integration Testing
- [ ] **Test**: Multi-step incremental restore flow
- [ ] **Verify**: Sequential processing of incremental backups
- [ ] **Check**: Proper state transitions and progress tracking

## 🔍 What to Keep vs Remove

### ✅ Keep (Essential Components):
- Event definitions in `tx_datashard.proto`
- Event classes in `datashard.h`
- Handler registration in `datashard_impl.h`
- Basic progress tracking in SchemeShard
- Integration with `TEvRunIncrementalRestore`

### ❌ Remove (Over-engineering):
- Complex DataShard validation logic
- Elaborate state machine in progress tracking
- `datashard_incremental_restore_request.cpp` (not needed)
- Complex error handling in DataShard

## 🎯 Success Criteria

1. **✅ Clean Build**: No compilation errors
2. **✅ Simple Flow**: DataShard acknowledges requests
3. **✅ Integration**: Works with existing `MultiIncrementalRestore`
4. **✅ Minimal Complexity**: Following build_index pattern
5. **✅ Tests Pass**: Basic unit tests succeed

## 🚀 Key Implementation Principles

### 1. **Leverage Existing Infrastructure**
- Use change senders for actual data movement
- Minimal state tracking in progress system
- Let `MultiIncrementalRestore` drive the process

### 2. **Follow build_index Pattern**
- Simple request/response between SchemeShard and DataShard
- DataShard just acknowledges, real work via existing mechanisms
- Minimal complexity in progress tracking

### 3. **Multi-Step Processing**
- Process incremental backups sequentially, one at a time
- Maintain chronological order of incremental backups
- Move to next incremental only when current is complete

## 📊 Implementation Priority

1. **HIGH**: Fix include path issue
2. **HIGH**: Implement proper state machine in SchemeShard
3. **MEDIUM**: Update DataShard response handling
4. **MEDIUM**: Integration with MultiIncrementalRestore
5. **LOW**: Add persistence for recovery
6. **LOW**: Add proper error handling and retries

## 🎯 Final Goal

A working implementation that:
- Processes multiple incremental backups sequentially
- Maintains proper state machine similar to build_index
- Integrates seamlessly with existing `MultiIncrementalRestore`
- Follows established YDB patterns
- Handles error cases and recovery

**This approach ensures incremental backups are applied in the correct chronological order, one at a time, which is critical for data consistency.**

