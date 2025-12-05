#pragma once

#include <ydb/core/tx/schemeshard/schemeshard_identificators.h>
#include <ydb/core/tx/schemeshard/schemeshard_path.h>

namespace NKikimr {

struct TPathId;

namespace NSchemeShard {

struct TOperationContext;
struct TTxState;

} // namespace NSchemeShard

} // namespace NKikimr

namespace NKikimrTxDataShard {

class TCreateCdcStreamNotice;

} // namespace NKikimrTxDataShard

namespace NKikimr::NSchemeShard::NCdcStreamAtTable {

void FillNotice(const TPathId& pathId, TOperationContext& context, NKikimrTxDataShard::TCreateCdcStreamNotice& notice);

void CheckWorkingDirOnPropose(const TPath::TChecker& checks, bool isTableIndex);
void CheckSrcDirOnPropose(
    const TPath::TChecker& checks,
    bool isInsideTableIndexPath,
    TTxId op = InvalidTxId);

} // namespace NKikimr::NSchemeShard::NCdcStreamAtTable

// Note: SyncIndexEntityVersion and SyncChildIndexes have been replaced by TVersionRegistry.
// Version synchronization is now done through ClaimVersionChange() for idempotent,
// sibling-coordinated version updates. See schemeshard_version_registry.h.

namespace NKikimr::NSchemeShard::NCdcStreamState {

} // namespace NKikimr::NSchemeShard::NCdcStreamState
