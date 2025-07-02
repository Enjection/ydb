#include "create_datashard_streaming_unit.h"
#include "execution_unit_ctors.h"

namespace NKikimr {
namespace NDataShard {

THolder<TExecutionUnit> CreateDataShardStreamingUnit(TDataShard &dataShard, TPipeline &pipeline) {
    return MakeHolder<TCreateDataShardStreamingUnit>(dataShard, pipeline);
}

} // namespace NDataShard
} // namespace NKikimr
