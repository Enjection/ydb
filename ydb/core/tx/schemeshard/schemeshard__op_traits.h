#pragma once

#include "schemeshard__operation.h"
#include "schemeshard_impl.h"
#include "schemeshard_path.h"

#include <ydb/core/protos/flat_scheme_op.pb.h>

#include <util/generic/map.h>
#include <util/generic/string.h>

#include <optional>
#include <utility>

namespace NKikimr::NSchemeShard {

using TTxTransaction = NKikimrSchemeOp::TModifyScheme;

enum EOperationClass {
    Create = 0,
    Alter,
    Drop,
    Other,
    Unknown
};

struct TSchemeTxTraitsFallback {
    constexpr inline static bool CreateDirsFromName = false;
    constexpr inline static bool CreateAdditionalDirs = false;
    constexpr inline static bool NeedRewrite = false;
    constexpr inline static EOperationClass Class = EOperationClass::Unknown;
};

template <NKikimrSchemeOp::EOperationType opType>
struct TSchemeTxTraits : public TSchemeTxTraitsFallback {};

template <>
struct TSchemeTxTraits<NKikimrSchemeOp::EOperationType::ESchemeOpMkDir>
    : public TSchemeTxTraitsFallback
{
    constexpr inline static bool CreateDirsFromName = true;
};

template <>
struct TSchemeTxTraits<NKikimrSchemeOp::EOperationType::ESchemeOpCreateTable>
    : public TSchemeTxTraitsFallback
{
    constexpr inline static bool CreateDirsFromName = true;
    constexpr inline static EOperationClass Class = EOperationClass::Create;

    static TVector<ISubOperation::TPtr> MakeOperationParts(
        const TOperation& op,
        const TTxTransaction& tx,
        TOperationContext& context);

    static void CollectChangingPaths(
        const TTxTransaction& tx,
        TVector<TString>& out);
};

template <>
struct TSchemeTxTraits<NKikimrSchemeOp::EOperationType::ESchemeOpCreatePersQueueGroup>
    : public TSchemeTxTraitsFallback
{
    constexpr inline static bool CreateDirsFromName = true;
};

template <>
struct TSchemeTxTraits<NKikimrSchemeOp::EOperationType::ESchemeOpCreateSubDomain>
    : public TSchemeTxTraitsFallback
{
    constexpr inline static bool CreateDirsFromName = true;
};

template <>
struct TSchemeTxTraits<NKikimrSchemeOp::EOperationType::ESchemeOpCreateExtSubDomain>
    : public TSchemeTxTraitsFallback
{
    constexpr inline static bool CreateDirsFromName = true;
};

template <>
struct TSchemeTxTraits<NKikimrSchemeOp::EOperationType::ESchemeOpCreateRtmrVolume>
    : public TSchemeTxTraitsFallback
{
    constexpr inline static bool CreateDirsFromName = true;
};

template <>
struct TSchemeTxTraits<NKikimrSchemeOp::EOperationType::ESchemeOpCreateBlockStoreVolume>
    : public TSchemeTxTraitsFallback
{
    constexpr inline static bool CreateDirsFromName = true;
};

template <>
struct TSchemeTxTraits<NKikimrSchemeOp::EOperationType::ESchemeOpCreateFileStore>
    : public TSchemeTxTraitsFallback
{
    constexpr inline static bool CreateDirsFromName = true;
};

template <>
struct TSchemeTxTraits<NKikimrSchemeOp::EOperationType::ESchemeOpCreateKesus>
    : public TSchemeTxTraitsFallback
{
    constexpr inline static bool CreateDirsFromName = true;
};

template <>
struct TSchemeTxTraits<NKikimrSchemeOp::EOperationType::ESchemeOpCreateSolomonVolume>
    : public TSchemeTxTraitsFallback
{
    constexpr inline static bool CreateDirsFromName = true;
};

template <>
struct TSchemeTxTraits<NKikimrSchemeOp::EOperationType::ESchemeOpCreateIndexedTable>
    : public TSchemeTxTraitsFallback
{
    constexpr inline static bool CreateDirsFromName = true;
};

template <>
struct TSchemeTxTraits<NKikimrSchemeOp::EOperationType::ESchemeOpCreateColumnStore>
    : public TSchemeTxTraitsFallback
{
    constexpr inline static bool CreateDirsFromName = true;
};

template <>
struct TSchemeTxTraits<NKikimrSchemeOp::EOperationType::ESchemeOpCreateColumnTable>
    : public TSchemeTxTraitsFallback
{
    constexpr inline static bool CreateDirsFromName = true;
};

template <>
struct TSchemeTxTraits<NKikimrSchemeOp::EOperationType::ESchemeOpCreateExternalTable>
    : public TSchemeTxTraitsFallback
{
    constexpr inline static bool CreateDirsFromName = true;
};

template <>
struct TSchemeTxTraits<NKikimrSchemeOp::EOperationType::ESchemeOpCreateExternalDataSource>
    : public TSchemeTxTraitsFallback
{
    constexpr inline static bool CreateDirsFromName = true;
};

template <>
struct TSchemeTxTraits<NKikimrSchemeOp::EOperationType::ESchemeOpCreateView>
    : public TSchemeTxTraitsFallback
{
    constexpr inline static bool CreateDirsFromName = true;
};

template <>
struct TSchemeTxTraits<NKikimrSchemeOp::EOperationType::ESchemeOpCreateResourcePool>
    : public TSchemeTxTraitsFallback
{
    constexpr inline static bool CreateDirsFromName = true;
};

template <>
struct TSchemeTxTraits<NKikimrSchemeOp::EOperationType::ESchemeOpCreateBackupCollection>
    : public TSchemeTxTraitsFallback
{
    constexpr inline static bool CreateDirsFromName = true;
};

template <>
struct TSchemeTxTraits<NKikimrSchemeOp::EOperationType::ESchemeOpBackupBackupCollection>
    : public TSchemeTxTraitsFallback
{
    constexpr inline static bool CreateAdditionalDirs = true;
    constexpr inline static bool NeedRewrite = true;
};

template <>
struct TSchemeTxTraits<NKikimrSchemeOp::EOperationType::ESchemeOpBackupIncrementalBackupCollection>
    : public TSchemeTxTraitsFallback
{
    constexpr inline static bool CreateAdditionalDirs = true;
    constexpr inline static bool NeedRewrite = true;
};

template <>
struct TSchemeTxTraits<NKikimrSchemeOp::EOperationType::ESchemeOpRestoreBackupCollection>
    : public TSchemeTxTraitsFallback
{
    constexpr inline static bool CreateAdditionalDirs = true;
};

template <>
struct TSchemeTxTraits<NKikimrSchemeOp::EOperationType::ESchemeOpCreateSequence>
    : public TSchemeTxTraitsFallback
{
    constexpr inline static bool CreateDirsFromName = true;
};

template <>
struct TSchemeTxTraits<NKikimrSchemeOp::EOperationType::ESchemeOpCreateReplication>
    : public TSchemeTxTraitsFallback
{
    constexpr inline static bool CreateDirsFromName = true;
};

template <>
struct TSchemeTxTraits<NKikimrSchemeOp::EOperationType::ESchemeOpCreateTransfer>
    : public TSchemeTxTraitsFallback
{
    constexpr inline static bool CreateDirsFromName = true;
};

template <>
struct TSchemeTxTraits<NKikimrSchemeOp::EOperationType::ESchemeOpCreateSecret>
    : public TSchemeTxTraitsFallback
{
    constexpr inline static bool CreateDirsFromName = true;
};

template <>
struct TSchemeTxTraits<NKikimrSchemeOp::EOperationType::ESchemeOpCreateStreamingQuery>
    : public TSchemeTxTraitsFallback
{
    constexpr inline static bool CreateDirsFromName = true;
};

template <>
struct TSchemeTxTraits<NKikimrSchemeOp::EOperationType::ESchemeOpCreateTestShardSet>
    : public TSchemeTxTraitsFallback
{
    constexpr inline static bool CreateDirsFromName = true;
};

namespace NOperation {

template <class TTraits>
std::optional<THashMap<TString, THashSet<TString>>> GetRequiredPaths(TTraits traits, const TTxTransaction& tx, const TOperationContext& context);

template <class TTraits>
bool Rewrite(TTraits traits, TTxTransaction& tx);

template <class TTraits>
std::optional<TString> GetTargetName(TTraits traits, const TTxTransaction& tx);

template <class TTraits>
bool SetName(TTraits traits, TTxTransaction& tx, const TString& name);

} // namespace NOperation

template <class TTraits>
std::optional<TString> GetTargetName(TTraits traits, const TTxTransaction& tx) {
    if constexpr (TTraits::CreateDirsFromName) {
        return NOperation::GetTargetName(traits, tx);
    }

    return {};
}

template <class TTraits>
bool SetName(TTraits traits, TTxTransaction& tx, const TString& name) {
    if constexpr (TTraits::CreateDirsFromName) {
        return NOperation::SetName(traits, tx, name);
    }

    return true;
}

template <class TTraits>
std::optional<THashMap<TString, THashSet<TString>>> GetRequiredPaths(TTraits traits, const TTxTransaction& tx, const TOperationContext& context) {
    if constexpr (TTraits::CreateAdditionalDirs) {
        return NOperation::GetRequiredPaths(traits, tx, context);
    }

    return {};
}

template <class TTraits>
bool Rewrite(TTraits traits, TTxTransaction& tx) {
    if constexpr (TTraits::NeedRewrite) {
        return NOperation::Rewrite(traits, tx);
    }

    return false;
}

bool IsCreatePathOperation(NKikimrSchemeOp::EOperationType op);

// Runtime snapshot of TSchemeTxTraits<> fields. When you add a field to
// TSchemeTxTraitsFallback, mirror it here and extend Describe() below.
struct TOpDescriptor {
    EOperationClass Class = EOperationClass::Unknown;
    bool CreateDirsFromName = false;
    bool CreateAdditionalDirs = false;
    bool NeedRewrite = false;
    bool HasMakeOperationParts = false;
    bool HasCollectChangingPaths = false;
};

template <class Traits>
constexpr TOpDescriptor Describe() {
    TOpDescriptor d;
    d.Class                = Traits::Class;
    d.CreateDirsFromName   = Traits::CreateDirsFromName;
    d.CreateAdditionalDirs = Traits::CreateAdditionalDirs;
    d.NeedRewrite          = Traits::NeedRewrite;
    d.HasMakeOperationParts = requires {
        Traits::MakeOperationParts(
            std::declval<const TOperation&>(),
            std::declval<const TTxTransaction&>(),
            std::declval<TOperationContext&>());
    };
    d.HasCollectChangingPaths = requires {
        Traits::CollectChangingPaths(
            std::declval<const TTxTransaction&>(),
            std::declval<TVector<TString>&>());
    };
    return d;
}

}  // namespace NKikimr::NSchemeShard
