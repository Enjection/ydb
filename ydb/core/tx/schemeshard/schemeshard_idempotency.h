#pragma once

#include "schemeshard_identificators.h"

#include <util/generic/string.h>
#include <util/generic/strbuf.h>
#include <util/generic/maybe.h>
#include <utility>

namespace Ydb::Operations {
class OperationParams;
}

namespace NKikimr::NSchemeShard {

TString GetUid(const Ydb::Operations::OperationParams& operationParams);

// All operation families keep independent indexes and native-record lifetimes.
template <typename TIndex, typename TKey>
const typename TIndex::mapped_type* FindOperationByUid(const TIndex& index, const TKey& key) {
    const auto it = index.find(key);
    return it == index.end() ? nullptr : &it->second;
}

struct TOperationUidIdentity {
    TMaybe<TPathId> DomainPathId;
    TMaybe<TStringBuf> UserSID;
    TMaybe<TStringBuf> RequestBody;
};

enum class EUidReplayMatch {
    Match,
    OwnerMismatch,
    DomainMismatch,
    RequestMismatch,
};

// Import/export compare domains; native SQL compares owner and DDL.
EUidReplayMatch CompareOperationUid(const TOperationUidIdentity& stored, const TOperationUidIdentity& requested);

// A UID belongs to an operation family on this SchemeShard tablet.
using TNativeOperationKey = std::pair<ui32, TString>;

struct TNativeOperationReplay {
    ui64 OperationId = 0;
    TString OriginalDdl;
    TString UserSID;
};

} // namespace NKikimr::NSchemeShard
