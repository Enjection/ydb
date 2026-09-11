#pragma once

#include <util/generic/strbuf.h>

namespace NKikimr::NBackup {

inline bool IsValidBackupOperationUid(TStringBuf key) {
    return !key.empty() && key.size() <= 128;
}

} // namespace NKikimr::NBackup
