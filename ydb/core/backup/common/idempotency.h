#pragma once

#include <util/generic/strbuf.h>

namespace NKikimr::NBackup {

inline bool IsValidNativeOperationUid(TStringBuf key) {
    if (key.empty() || key.size() > 256) {
        return false;
    }
    for (const unsigned char c : key) {
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == ':')) {
            return false;
        }
    }
    return true;
}

} // namespace NKikimr::NBackup
