#pragma once
#include "encryption.h"

#include <ydb/core/base/row_version.h>

#include <util/generic/map.h>
#include <util/generic/maybe.h>
#include <util/generic/ptr.h>
#include <util/generic/string.h>

namespace NKikimr::NBackup {

using TVirtualTimestamp = TRowVersion;

enum class EStorageType {
    YT,
    S3,
};

struct TLogMetadata : TSimpleRefCount<TLogMetadata> {
    using TPtr = TIntrusivePtr<TLogMetadata>;

    const TVirtualTimestamp StartVts;
    TString ConsistencyKey;
    EStorageType StorageType;
    TString StoragePath;
};

struct TFullBackupMetadata : TSimpleRefCount<TFullBackupMetadata> {
    using TPtr = TIntrusivePtr<TFullBackupMetadata>;

    const TVirtualTimestamp SnapshotVts;
    TString ConsistencyKey;
    TLogMetadata::TPtr FollowingLog;
    EStorageType StorageType;
    TString StoragePath;
};

struct TChangefeedMetadata {
    TString ExportPrefix;
    TString Name;
};

class TMetadata {
public:
    TMetadata() = default;
    TMetadata(TVector<TFullBackupMetadata::TPtr>&& fullBackups, TVector<TLogMetadata::TPtr>&& logs);

    void AddFullBackup(TFullBackupMetadata::TPtr fullBackup);
    void AddLog(TLogMetadata::TPtr log);
    void SetConsistencyKey(const TString& key);
    void SetVersion(ui64 version);
    bool HasVersion() const;
    ui64 GetVersion() const;
    void AddChangefeed(const TChangefeedMetadata& changefeed);
    const TMaybe<std::vector<TChangefeedMetadata>>& GetChangefeeds() const;

    TString Serialize() const;
    static TMetadata Deserialize(const TString& metadata);
private:
    TString ConsistencyKey;
    TMap<TVirtualTimestamp, TFullBackupMetadata::TPtr> FullBackups;
    TMap<TVirtualTimestamp, TLogMetadata::TPtr> Logs;
    TMaybe<std::vector<TChangefeedMetadata>> Changefeeds;
    TMaybeFail<ui64> Version;
};

TString NormalizeItemPath(const TString& path);
TString NormalizeItemPrefix(TString prefix);
TString NormalizeExportPrefix(TString prefix);

class TSchemaMapping {
public:
    struct TItem {
        TString ExportPrefix;
        TString ObjectPath;
        TMaybe<NBackup::TEncryptionIV> IV;
    };

    TSchemaMapping() = default;

    TString Serialize() const;
    bool Deserialize(const TString& jsonContent, TString& error);

public:
    std::vector<TItem> Items;
};

} // namespace NKikimr::NBackup
