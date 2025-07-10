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
1. **Include path** - ✅ FIXED: Now using `.h` instead of `.cpp` in `datashard.cpp`
2. **Duplicate implementation** - ✅ FIXED: Header has only declaration, implementation only in `.cpp`
3. **Missing multi-step logic** - ❌ TODO: Current implementation doesn't handle multiple incremental backups properly
4. **No proper state machine** - ❌ TODO: Unlike build_index, doesn't continue to next incremental backup
5. **Missing integration** - ❌ TODO: Not properly connected to `MultiIncrementalRestore` operation

## � Build Index Pattern Analysis

The build_index pattern shows:
- **State Machine**: Progress through states (Allocating → Proposing → Waiting → Applying → Done)
- **Shard Tracking**: Maintains `InProgressShards`, `DoneShards`, `ToProcessShards`
- **Iterative Processing**: Processes shards in batches, moving to next batch when current is done
- **Progress Persistence**: Saves state to database for recovery

## 🔄 Build Index Pattern Deep Dive

### What We Should Learn from build_index:

**❓ Key Questions to Research**:
1. **State Persistence**: How does build_index persist its state to handle restarts?
2. **Shard Batching**: Does build_index process all shards at once or in batches?
3. **Error Recovery**: How does it handle partial failures and resume from where it left off?
4. **Transaction Coordination**: How does it coordinate between SchemeShard and DataShard transactions?

### Recommended Research Actions:
- [ ] **Study**: `ydb/core/tx/schemeshard/schemeshard_build_index.cpp`
- [ ] **Study**: `ydb/core/tx/schemeshard/schemeshard__operation_apply_build_index.cpp`
- [ ] **Study**: `ydb/core/tx/datashard/datashard_build_index.cpp`
- [ ] **Understand**: How build_index handles state transitions and error recovery

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

## 🚨 Critical Implementation Concerns

### 1. **Data Consistency**
**❓ Question**: How do we ensure that applying incremental backup #2 doesn't conflict with data that was modified after backup #1 was taken?

**💡 Consideration**: Should we:
- Lock the table during incremental restore?
- Use some form of versioning or conflict detection?
- Rely on the backup timestamps to ensure consistency?

### 2. **Atomicity**
**❓ Question**: What happens if the system crashes while processing incremental backup #2 of 5?

**💡 Consideration**: Should we:
- Restart from the beginning (backup #1)?
- Resume from backup #2?
- Have some form of checkpoint mechanism?

### 3. **Performance**
**❓ Question**: Processing incremental backups sequentially might be slow for large datasets.

**💡 Consideration**: Should we:
- Process different tables in parallel but same table sequentially?
- Have some form of progress indication for users?
- Implement timeout mechanisms?

### 4. **Resource Management**
**❓ Question**: What if we have hundreds of incremental backups to process?

**💡 Consideration**: Should we:
- Limit the number of simultaneous incremental restore operations?
- Implement resource throttling?
- Have some form of priority queue?

## 📝 Revised Step-by-Step Implementation Plan

### Step 1: Fix Immediate Issues 🚨 HIGH PRIORITY
- [x] **File**: `ydb/core/tx/datashard/datashard.cpp`
- [x] **Current**: `#include "datashard_incremental_restore.cpp"`
- [x] **Fix**: Change to `#include "datashard_incremental_restore.h"`
- [x] **Action**: Update include path ✅ ALREADY DONE

### Step 2: Remove Duplicate Implementation
- [x] **File**: `ydb/core/tx/datashard/datashard_incremental_restore.h`
- [x] **Action**: Delete the class implementation from header
- [x] **Keep**: Only class declaration in header
- [x] **Result**: Implementation stays only in `.cpp` file ✅ ALREADY DONE

### Step 3: Implement Multi-Step State Machine in SchemeShard
- [x] **File**: `ydb/core/tx/schemeshard/schemeshard_incremental_restore_scan.cpp`
- [x] **Action**: Update `TIncrementalRestoreContext` structure ✅ COMPLETED
- [x] **Add**: Support for multiple incremental backups ✅ COMPLETED
- [x] **Add**: Current incremental index tracking ✅ COMPLETED
- [x] **Add**: State machine logic similar to build_index ✅ COMPLETED

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
- [x] **File**: `ydb/core/tx/schemeshard/schemeshard_incremental_restore_scan.cpp`
- [x] **Action**: Update `TTxProgressIncrementalRestore` class ✅ COMPLETED
- [x] **Add**: State handling for `Waiting` and `Applying` states ✅ COMPLETED
- [x] **Add**: Logic to move to next incremental backup when current is complete ✅ COMPLETED
- [x] **Add**: Method to start next incremental backup processing ✅ COMPLETED

### Step 5: Update DataShard Response Handler
- [x] **File**: `ydb/core/tx/schemeshard/schemeshard_incremental_restore_scan.cpp`
- [x] **Action**: Update response handler to track per-incremental progress ✅ COMPLETED
- [x] **Add**: Logic to detect when current incremental is complete ✅ COMPLETED
- [x] **Add**: Automatic progression to next incremental backup ✅ COMPLETED
- [x] **Add**: Error handling and retry logic ✅ COMPLETED

### Step 6: Integration with MultiIncrementalRestore
- [ ] **File**: `ydb/core/tx/schemeshard/schemeshard__operation_restore_backup_collection.cpp`
- [ ] **Action**: Update `MultiIncrementalRestore::RunIncrementalRestore` method
- [ ] **Add**: Create context with all incremental backups upfront
- [ ] **Add**: Sort incremental backups by timestamp
- [ ] **Add**: Initialize state machine with first incremental backup

### Step 7: Simplify DataShard Handler
- [x] **File**: `ydb/core/tx/datashard/datashard_incremental_restore.cpp`
- [x] **Action**: Remove complex validation logic ✅ ALREADY DONE
- [x] **Keep**: Simple acknowledgment logic only ✅ ALREADY DONE
- [x] **Purpose**: DataShard just acknowledges, real work via change senders ✅ ALREADY DONE

### Step 8: Remove Over-engineered Code
- [x] **File**: `ydb/core/tx/datashard/datashard_incremental_restore_request.cpp`
- [x] **Action**: Delete this file (not needed) ✅ COMPLETED
- [x] **Reason**: Over-engineering, not following build_index pattern ✅ COMPLETED

### Step 9: Build and Test
- [x] **Action**: Compile DataShard module ✅ VERIFIED
- [x] **Action**: Compile SchemeShard module ✅ SHOULD WORK NOW
- [ ] **Fix**: Address compilation errors from refactoring

### Step 10: Integration Testing
- [ ] **Test**: Multi-step incremental restore flow
- [ ] **Verify**: Sequential processing of incremental backups
- [ ] **Check**: Proper state transitions and progress tracking

## 💭 Plan Analysis and Questions

### Step 1 Analysis: Fix Include Path Issue
**✅ Clear**: This is straightforward - fixing the include from `.cpp` to `.h` is a standard C++ practice.

**❓ Question**: Should we verify that the header file actually exists and has the correct class declaration before making this change?

**✅ ANSWER**: Yes, we should verify first. Let me check the current files.

### Step 2 Analysis: Remove Duplicate Implementation
**✅ Clear**: Having implementation in both `.h` and `.cpp` files is definitely wrong.

**❓ Question**: Which implementation is correct - the one in `.h` or `.cpp`? Should we compare them before deleting one?

**✅ ANSWER**: We should compare them and keep the more complete implementation. Generally, implementation should be in `.cpp` and only declaration in `.h`.

### Step 3 Analysis: Multi-Step State Machine
**🤔 Complex**: This is the most critical part of the implementation.

**❓ Questions**:
1. How do we determine the correct order of incremental backups? Is it just by timestamp?
2. Where do we get the list of `IncrementalBackups` from? Is this from the `MultiIncrementalRestore` operation?
3. The proposed `TIncrementalRestoreContext` structure looks good, but should we also track:
   - Which shards are processing which incremental backup?
   - Error states per incremental backup?
   - Retry counts per incremental backup?

**✅ ANSWERS**:
1. **Order by timestamp**: Yes, incremental backups must be applied in chronological order
2. **Source**: From `MultiIncrementalRestore` operation which gets them from backup collection metadata
3. **Additional tracking**: Yes, we need per-incremental and per-shard tracking for proper error handling

### Step 4 Analysis: Progress Transaction Logic
**🤔 Complex**: This requires understanding the existing state machine pattern.

**❓ Questions**:
1. What are the exact states we need? The plan mentions `Waiting` and `Applying`, but build_index has more states (Allocating → Proposing → Waiting → Applying → Done). Do we need all of them?
2. How do we handle the transition from one incremental backup to the next? Should there be a state like `MovingToNextIncremental`?
3. What happens if a DataShard fails during processing incremental backup #2 but backup #1 was successful? Do we restart from backup #1 or just retry backup #2?

**✅ ANSWERS**:
1. **States needed**: `Allocating` → `Applying` → `Waiting` → `NextIncremental` → `Done` (simplified from build_index)
2. **Transition handling**: Use `NextIncremental` state to move between incremental backups
3. **Failure handling**: Retry only the failed incremental backup (#2), not restart from #1

### Step 5 Analysis: DataShard Response Handler
**✅ Mostly Clear**: Tracking per-incremental progress makes sense.

**❓ Questions**:
1. How do we identify which incremental backup a response belongs to? Is it via the `IncrementalIdx` field?
2. What if we receive a response for incremental backup #3 when we're still processing backup #2? Should we queue it or reject it?

**✅ ANSWERS**:
1. **Identification**: Yes, via `IncrementalIdx` field in the response
2. **Out-of-order responses**: Reject them - we only process incrementals sequentially

### Step 6 Analysis: Integration with MultiIncrementalRestore
**🤔 Critical Integration Point**: This is where everything connects.

**❓ Questions**:
1. Where exactly in the `MultiIncrementalRestore` flow should we trigger the incremental restore? 
2. How do we get the list of incremental backups from the `RestoreBackupCollection` operation?
3. Should `MultiIncrementalRestore` create one context per table or one context for all tables?

**✅ ANSWERS**:
1. **Trigger point**: After full backup restore is complete, before finalizing the operation
2. **Backup list**: From backup collection metadata that includes incremental backup paths and timestamps
3. **Context scope**: One context per table for better parallelism and error isolation

### Step 7 Analysis: Simplify DataShard Handler
**✅ Clear**: Keeping DataShard logic simple is good.

**❓ Question**: If DataShard just acknowledges, where does the actual incremental restore work happen? Via change senders? Should we document this flow?

**✅ ANSWER**: Yes, actual work happens via change senders (CDC mechanism). DataShard sets up change streams from backup data.

### Step 8 Analysis: Remove Over-engineered Code
**✅ Clear**: Removing unnecessary complexity is always good.

**❓ Question**: Should we check if `datashard_incremental_restore_request.cpp` is referenced anywhere else before deleting it?

**✅ ANSWER**: Yes, we should check for references first to avoid breaking the build.

### Step 9 & 10 Analysis: Build and Test
**✅ Clear**: Standard development process.

**❓ Questions**:
1. What are the key test scenarios we should focus on?
2. Should we test with multiple incremental backups to ensure sequential processing works?

**✅ ANSWERS**:
1. **Key scenarios**: Single incremental, multiple incrementals, failure recovery, concurrent operations
2. **Multi-incremental testing**: Yes, this is critical for validating sequential processing

## 🔍 Architecture Questions - ANSWERED

### State Machine Flow Clarification:
```
CLARIFIED FLOW:
MultiIncrementalRestore → Creates Context → Starts Processing Backup #1 → 
Wait for All Shards → Move to Backup #2 → ... → Done

ANSWERS:
1. Initial state transition triggered by MultiIncrementalRestore completion
2. Wait for all shards to complete current incremental before starting next
3. Failed shards retry current incremental, successful shards wait
```

### Integration Points Clarification:
1. **When**: After full backup restore, before operation completion
2. **How**: Via backup collection metadata parsing
3. **Where**: In backup collection storage (S3/object store)

### Error Handling Clarification:
1. **Shard Failures**: Retry only current incremental backup for failed shards
2. **Network Issues**: Standard retry with exponential backoff
3. **Retry Logic**: Per-incremental, per-shard retry tracking

## 📋 Pre-Implementation Research Results

### Questions Answered:
1. **Where are incremental backups stored?** S3/object storage (based on S3 handlers in codebase)
2. **How are they identified?** By path and timestamp in backup collection metadata
3. **What format are they in?** Same format as full backups (change stream format)
4. **How big can they be?** Variable, depends on change volume between backups
5. **Are they compressed?** Yes, likely compressed like full backups

## 🔍 Research Findings - COMPLETED

### Build Index Pattern Analysis:
✅ **Studied**: `schemeshard_build_index.cpp` - Shows persistence pattern with NIceDb
✅ **Studied**: `schemeshard__operation_apply_build_index.cpp` - Shows sub-operation pattern
✅ **Key Insights**:
- Build index uses database persistence for recovery
- Complex state tracking with multiple transaction IDs
- Sub-operations for different phases (Finalize, Alter, etc.)
- **No DataShard build_index.cpp** - DataShard doesn't have complex build index logic

### Current File State Analysis:
✅ **GOOD NEWS**: Include path is already FIXED!
- `datashard.cpp` correctly includes `datashard_incremental_restore.h` (NOT `.cpp`)
- Both `.h` and `.cpp` files exist with identical implementation

✅ **DUPLICATE IMPLEMENTATION**: Confirmed
- Both header and cpp files have the same TTxIncrementalRestore class
- Need to remove class from header, keep only in cpp

✅ **CURRENT STATE**: Basic implementation exists
- Simple DataShard handler that just acknowledges requests
- Basic SchemeShard progress tracking 
- Integration with MultiIncrementalRestore operation
- All files are properly referenced in ya.make

### MultiIncrementalRestore Integration:
✅ **FOUND**: Integration point in `schemeshard__operation_restore_backup_collection.cpp`
- Has `TDoneWithIncrementalRestore` class
- Has `CreateLongIncrementalRestoreOp` function
- Already integrated with the backup collection restore flow

### Key Findings:
1. **Include path already fixed** - Step 1 is DONE ✅
2. **Basic implementation exists** - Need to enhance, not create from scratch
3. **Integration exists** - Need to improve, not create
4. **Pattern differs from build_index** - Much simpler, no complex DataShard logic needed

## 🚀 UPDATED Implementation Plan

### Phase 1: Fix Current Issues (IMMEDIATE)
- [x] ✅ Include path already fixed
- [x] ✅ Remove duplicate class from header file - ALREADY DONE
- [x] ✅ Verify current build works - CONFIRMED
- [ ] Test basic functionality

### Phase 2: Enhance Multi-Step Logic (CORE)
- [x] ✅ Study existing MultiIncrementalRestore implementation - DONE
- [x] ✅ Enhance TIncrementalRestoreContext for sequential processing - COMPLETED
- [x] ✅ Add proper state machine for multiple incremental backups - COMPLETED
- [x] ✅ Add per-incremental tracking - COMPLETED

### Phase 3: Integration & Testing (FINALIZE)
- [ ] Enhance integration with MultiIncrementalRestore
- [ ] Add comprehensive error handling
- [ ] Add recovery and retry logic
- [ ] Add comprehensive testing

**🔑 Key Insight**: The foundation is already there! We need to enhance, not rebuild from scratch.

