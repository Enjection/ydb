#pragma once
#include "datashard_impl.h"

namespace NKikimr {
namespace NDataShard {

// Forward declaration - implementation is in datashard_incremental_restore.cpp
class TDataShard::TTxIncrementalRestore : public NTabletFlatExecutor::TTransactionBase<TDataShard> {
public:
    TTxIncrementalRestore(TDataShard* self, TEvDataShard::TEvIncrementalRestoreRequest::TPtr& ev);
    
    bool Execute(TTransactionContext&, const TActorContext& ctx) override;
    void Complete(const TActorContext& ctx) override;

private:
    TEvDataShard::TEvIncrementalRestoreRequest::TPtr Event;
};

} // namespace NDataShard
} // namespace NKikimr
