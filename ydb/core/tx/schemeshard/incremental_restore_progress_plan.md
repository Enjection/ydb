# 📋 Incremental Restore Implementation Plan

## 🎯 Current Status Analysis

### ✅ Already Implemented (from changes.diff):
- ✅ Event definitions in `tx_datashard.proto`
- ✅ Event classes in `datashard.h`  
- ✅ Handler registration in `datashard_impl.h`
- ✅ Basic handler stub in `datashard.cpp`
- ✅ Transaction type added to `counters_schemeshard.proto`

### ❌ Critical Issues Found:
1. **🚨 Syntax Error**: Commas removed from proto file (lines 494-502)
2. **🔧 Over-engineering**: Complex DataShard validation logic unnecessary
3. **🔗 Missing Integration**: No clear connection to existing `MultiIncrementalRestore`
4. **📁 File Structure**: Incorrect include path in `datashard.cpp`

## 🏗️ Simplified Architecture

Following the build_index pattern, the implementation should be:

```
User Request → RestoreBackupCollection → MultiIncrementalRestore → Change Senders
                                              ↓
                                      Progress Tracking (minimal)
                                              ↓
                                      DataShard Handlers (simple)
```

### Core Principle: **MultiIncrementalRestore is the Primary Driver**

The existing `MultiIncrementalRestore` operation in `schemeshard__operation_restore_backup_collection.cpp` should orchestrate the entire process, with minimal additional complexity.

## 📝 Step-by-Step Implementation Plan

### Step 1: Fix Proto Syntax Error 🚨 HIGH PRIORITY
- [ ] **File**: `ydb/core/protos/counters_schemeshard.proto`
- [ ] **Lines**: 494-502 (Ranges definitions)
- [ ] **Action**: Restore commas after each `Ranges:` entry
- [ ] **Fix**: Change `Ranges: { Value: 0 Name: "0" }` to `Ranges: { Value: 0 Name: "0" },`

### Step 2: Simplify DataShard Handler Implementation
- [ ] **File**: `ydb/core/tx/datashard/datashard_incremental_restore.cpp`
- [ ] **Action**: Replace with minimal handler (no complex validation)
- [ ] **Purpose**: Just acknowledge requests and defer to change senders

```cpp
// Simplified handler approach
class TDataShard::TTxIncrementalRestore : public TTransactionBase<TDataShard> {
    // Simple acknowledgment logic only
    // Real work happens via change senders
};
```

### Step 3: Fix Include Path
- [ ] **File**: `ydb/core/tx/datashard/datashard.cpp`
- [ ] **Current**: `#include "datashard_incremental_restore.cpp"`
- [ ] **Fix**: Change to `#include "datashard_incremental_restore.h"`
- [ ] **Create**: Header file with class declaration

### Step 4: Create Proper Header File
- [ ] **File**: Create `ydb/core/tx/datashard/datashard_incremental_restore.h`
- [ ] **Content**: Class declaration for `TTxIncrementalRestore`
- [ ] **Include**: Proper forward declarations

### Step 5: Update Build System
- [ ] **File**: `ydb/core/tx/datashard/ya.make`
- [ ] **Action**: Add `datashard_incremental_restore.cpp` to SRCS() section
- [ ] **Check**: Verify build configuration

### Step 6: Verify SchemeShard Integration
- [ ] **File**: `ydb/core/tx/schemeshard/schemeshard_impl.cpp`
- [ ] **Check**: Ensure handler registration exists
- [ ] **Verify**: `TTxProgressIncrementalRestore` is properly connected

### Step 7: Connect to MultiIncrementalRestore
- [ ] **File**: `ydb/core/tx/schemeshard/schemeshard__operation_restore_backup_collection.cpp`
- [ ] **Verify**: Progress tracking integration
- [ ] **Check**: `TEvRunIncrementalRestore` flow

### Step 8: Add Response Handler
- [ ] **File**: `ydb/core/tx/schemeshard/schemeshard_incremental_restore_scan.cpp`
- [ ] **Action**: Add `TEvIncrementalRestoreResponse` handler
- [ ] **Purpose**: Process DataShard responses

### Step 9: Build and Test
- [ ] **Action**: Compile DataShard module
- [ ] **Action**: Compile SchemeShard module
- [ ] **Fix**: Address compilation errors

### Step 10: Basic Unit Tests
- [ ] **File**: Create `datashard_ut_incremental_restore.cpp`
- [ ] **Test**: Basic request/response flow
- [ ] **Verify**: Handler acknowledgment

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

### 3. **Fix Critical Issues First**
- Proto syntax error blocks compilation
- File structure issues prevent proper building
- Focus on making it work, then optimize

## 📊 Implementation Timeline

### Phase 1: Fix Critical Issues (1-2 hours)
- Fix proto syntax error
- Fix include paths
- Ensure clean compilation

### Phase 2: Simplify Implementation (2-3 hours)
- Replace complex DataShard logic
- Streamline SchemeShard integration
- Basic testing

### Phase 3: Integration Testing (1-2 hours)
- End-to-end flow validation
- Error handling verification
- Performance check

## 🎯 Final Goal

A working, minimal implementation that:
- Compiles without errors
- Handles incremental restore requests
- Integrates with existing `MultiIncrementalRestore`
- Follows established YDB patterns
- Is ready for production use

**Total estimated time: 4-7 hours of focused development**

