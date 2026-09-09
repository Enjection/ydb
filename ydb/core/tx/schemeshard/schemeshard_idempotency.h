#pragma once

#include "schemeshard_identificators.h"

#include <util/generic/string.h>
#include <utility>

namespace NKikimr::NSchemeShard {

// A UID belongs to an operation family on this SchemeShard tablet.
using TNativeOperationKey = std::pair<ui32, TString>;

struct TNativeOperationReplay {
    ui64 OperationId = 0;
    TPathId DomainPathId;
    TString OriginalDdl;
    TString UserSID;
};

} // namespace NKikimr::NSchemeShard
