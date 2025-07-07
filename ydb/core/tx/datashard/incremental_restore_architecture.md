# YDB Incremental Restore Architecture - Critical Understanding

## IMPORTANT: Transaction Target vs Source Semantics

### Key Understanding (July 3, 2025)
**The transaction is sent TO the target table (destination), not the source table (backup).**

In incremental restore operations:
- **Source Table**: The backup table containing the incremental data (e.g., `/Root/.backups/collections/MyCollection/19700101000002Z_incremental/Table`)
- **Target Table**: The destination table where data is being restored (e.g., `/Root/Table`)

### Transaction Processing Flow
1. **Transaction Target**: The destination table (`/Root/Table`) - this is where the transaction is executed
2. **Transaction Source Reference**: The backup table (`/Root/.backups/.../Table`) - this is where data is read from
3. **DataShard Processing**: The transaction is sent to the DataShards that host the destination table
4. **Data Flow**: Data flows FROM source (backup) TO target (destination)

### CreateTx Parameters
```cpp
auto& txState = context.SS->CreateTx(OperationId, txType, targetPathId, sourcePathId);
```

Where:
- **targetPathId**: Destination table PathId (where transaction executes) - CORRECT in current code
- **sourcePathId**: Backup table PathId (where data comes from) - CORRECT in current code

### Current Issue Analysis
The log shows:
```
CreateTx for txid 281474976710757:0 type: TxRestoreIncrementalBackupAtTable 
target path: [OwnerId: 72057594046644480, LocalPathId: 12] 
source path: [OwnerId: 72057594046644480, LocalPathId: 15]
```

- **target path (12)**: This is the backup table - CORRECT (transaction target)
- **source path (15)**: This is the destination table - CORRECT (data source reference)

### The Real Issue
The problem is NOT in the CreateTx parameter order. The issue is in the protobuf parsing:

```
Available srcPathId[0]: <Invalid>
```

The protobuf `RestoreOp.GetSrcPathIds(0)` is returning `<Invalid>` instead of the expected backup table PathId (12).

This suggests the issue is in how the protobuf message is constructed in TTxProgress::OnAllocateResult, not in the transaction state creation.

### Next Steps
1. ✅ Understand transaction semantics correctly
2. 🔄 Fix the protobuf parsing issue in FillNotice method
3. 🔄 Ensure TTxProgress creates correct protobuf messages
