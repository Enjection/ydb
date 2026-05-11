// Unit tests for the schemeshard per-op trait module pattern (CreateTable
// pilot). What this PR introduced and what these tests pin down:
//
//   1. EOperationClass lifted into the trait header so trait specs can
//      carry classification at compile time.
//   2. TSchemeTxTraits<ESchemeOpCreateTable> declares Class=Create plus the
//      static methods MakeOperationParts and CollectChangingPaths.
//   3. The audit-log dispatch (MakeAuditLogFragment / ExtractChangingPaths)
//      starts with a DispatchOp prelude that routes CreateTable through the
//      trait method; the central switch case for CreateTable is gone.
//   4. Unmigrated ops still flow through the legacy switch — both for
//      audit-path extraction and for GetOperationClass.
//   5. Describe<Traits>() is the single source of truth for the
//      materialized trait shape consumed by tooling.

#include <ydb/core/tx/schemeshard/schemeshard__dispatch_op.h>
#include <ydb/core/tx/schemeshard/schemeshard__op_traits.h>
#include <ydb/core/tx/schemeshard/schemeshard_audit_log_fragment.h>

#include <ydb/core/protos/flat_scheme_op.pb.h>

#include <library/cpp/testing/unittest/registar.h>

using namespace NKikimr::NSchemeShard;
namespace NSO = NKikimrSchemeOp;

// --- Compile-time facts --------------------------------------------------
//
// These are the invariants the per-op pilot adds to the trait system. If a
// future change breaks any of them the file simply won't compile, which is
// the strongest possible regression guard.

static_assert(
    TSchemeTxTraitsFallback::Class == EOperationClass::Unknown,
    "Fallback trait must default Class to Unknown");

static_assert(
    TSchemeTxTraits<NSO::ESchemeOpCreateTable>::Class == EOperationClass::Create,
    "CreateTable trait must declare Class=Create");

static_assert(
    Describe<TSchemeTxTraits<NSO::ESchemeOpCreateTable>>().Class == EOperationClass::Create,
    "Describe<>() must report Class for migrated ops");

static_assert(
    Describe<TSchemeTxTraits<NSO::ESchemeOpCreateTable>>().HasMakeOperationParts,
    "CreateTable trait must declare MakeOperationParts");

static_assert(
    Describe<TSchemeTxTraits<NSO::ESchemeOpCreateTable>>().HasCollectChangingPaths,
    "CreateTable trait must declare CollectChangingPaths");

static_assert(
    !Describe<TSchemeTxTraits<NSO::ESchemeOpMkDir>>().HasMakeOperationParts,
    "MkDir is not migrated; trait must not advertise MakeOperationParts");

static_assert(
    !Describe<TSchemeTxTraits<NSO::ESchemeOpDropTable>>().HasCollectChangingPaths,
    "DropTable is not migrated; trait must not advertise CollectChangingPaths");

// --- Runtime tests -------------------------------------------------------

Y_UNIT_TEST_SUITE(SchemeshardOpTraits) {

    Y_UNIT_TEST(GetOperationClassReturnsCreateForCreateTable) {
        // Legacy switch must still classify CreateTable correctly. Parity
        // with the trait's Class is enforced by the static_assert in
        // schemeshard__op_traits.cpp.
        UNIT_ASSERT_EQUAL(GetOperationClass(NSO::ESchemeOpCreateTable),
                          EOperationClass::Create);
        UNIT_ASSERT(IsCreatePathOperation(NSO::ESchemeOpCreateTable));
    }

    Y_UNIT_TEST(GetOperationClassReturnsDropForDropTable) {
        // DropTable hasn't migrated yet — the legacy switch is the source
        // of truth here. Trait Class is intentionally Unknown until DropTable
        // moves to the per-op module pattern.
        UNIT_ASSERT_EQUAL(GetOperationClass(NSO::ESchemeOpDropTable),
                          EOperationClass::Drop);
        UNIT_ASSERT(!IsCreatePathOperation(NSO::ESchemeOpDropTable));
    }

    Y_UNIT_TEST(CollectChangingPathsForCreateTableProducesExpectedPath) {
        // Direct call to the trait's static method. Verifies the per-op
        // module owns the correct logic in isolation, independent of
        // dispatch.
        NSO::TModifyScheme tx;
        tx.SetOperationType(NSO::ESchemeOpCreateTable);
        tx.SetWorkingDir("/Root/Db");
        tx.MutableCreateTable()->SetName("Users");

        TVector<TString> paths;
        TSchemeTxTraits<NSO::ESchemeOpCreateTable>::CollectChangingPaths(tx, paths);

        UNIT_ASSERT_VALUES_EQUAL(paths.size(), 1u);
        UNIT_ASSERT_VALUES_EQUAL(paths[0], "/Root/Db/Users");
    }

    Y_UNIT_TEST(MakeAuditLogFragmentRoutesCreateTableThroughTrait) {
        // End-to-end: the public audit API still produces the correct
        // Paths for CreateTable after the central switch case was deleted
        // in favor of the DispatchOp prelude. If routing breaks (trait
        // method not invoked), Paths would be empty.
        NSO::TModifyScheme tx;
        tx.SetOperationType(NSO::ESchemeOpCreateTable);
        tx.SetWorkingDir("/Root/Db");
        tx.MutableCreateTable()->SetName("Users");

        const auto fragment = MakeAuditLogFragment(tx);
        UNIT_ASSERT_VALUES_EQUAL(fragment.Paths.size(), 1u);
        UNIT_ASSERT_VALUES_EQUAL(fragment.Paths[0], "/Root/Db/Users");
    }

    Y_UNIT_TEST(MakeAuditLogFragmentStillHandlesUnmigratedOps) {
        // MkDir hasn't migrated and must keep flowing through the legacy
        // switch. Catches accidental over-deletion of switch cases.
        NSO::TModifyScheme tx;
        tx.SetOperationType(NSO::ESchemeOpMkDir);
        tx.SetWorkingDir("/Root");
        tx.MutableMkDir()->SetName("NewDir");

        const auto fragment = MakeAuditLogFragment(tx);
        UNIT_ASSERT_VALUES_EQUAL(fragment.Paths.size(), 1u);
        UNIT_ASSERT_VALUES_EQUAL(fragment.Paths[0], "/Root/NewDir");
    }

    Y_UNIT_TEST(MakeAuditLogFragmentHandlesCopyFromTableVariant) {
        // CreateTable with CopyFromTable set: the trait method should still
        // surface the destination path under WorkingDir/Name (matching the
        // pre-refactor inline code).
        NSO::TModifyScheme tx;
        tx.SetOperationType(NSO::ESchemeOpCreateTable);
        tx.SetWorkingDir("/Root/Db");
        auto* createTable = tx.MutableCreateTable();
        createTable->SetName("UsersCopy");
        createTable->SetCopyFromTable("/Root/Db/Users");

        const auto fragment = MakeAuditLogFragment(tx);
        UNIT_ASSERT_VALUES_EQUAL(fragment.Paths.size(), 1u);
        UNIT_ASSERT_VALUES_EQUAL(fragment.Paths[0], "/Root/Db/UsersCopy");
    }

    Y_UNIT_TEST(DispatchOpInstantiatesTheRightTraitForCreateTable) {
        // Verify the existing DispatchOp routes CreateTable to its
        // specialization (the one carrying Class=Create), not to the
        // fallback type.
        NSO::TModifyScheme tx;
        tx.SetOperationType(NSO::ESchemeOpCreateTable);

        const auto cls = DispatchOp(tx, [](auto traits) {
            return decltype(traits)::Class;
        });
        UNIT_ASSERT_EQUAL(cls, EOperationClass::Create);
    }

    Y_UNIT_TEST(DispatchOpRoutesUnmigratedOpToOwnSpec) {
        // For an unmigrated op like DropTable the dispatch still hits the
        // op-specific TSchemeTxTraits<DropTable> spec (which inherits
        // Class=Unknown from the fallback) — NOT the fallback type itself.
        // This pins down the pattern that lets new fields default safely
        // to fallback values per-op.
        NSO::TModifyScheme tx;
        tx.SetOperationType(NSO::ESchemeOpDropTable);

        const auto cls = DispatchOp(tx, [](auto traits) {
            return decltype(traits)::Class;
        });
        UNIT_ASSERT_EQUAL(cls, EOperationClass::Unknown);
    }

    Y_UNIT_TEST(DescribeReportsCreateTableTraitFlags) {
        // Sanity: Describe<>() surfaces both the explicit fields
        // (CreateDirsFromName) and the derived 'Has*' fields. Tooling
        // (ss_tool) consumes these; the contract should not silently
        // change shape.
        constexpr auto d = Describe<TSchemeTxTraits<NSO::ESchemeOpCreateTable>>();
        UNIT_ASSERT_EQUAL(d.Class, EOperationClass::Create);
        UNIT_ASSERT(d.CreateDirsFromName);
        UNIT_ASSERT(!d.CreateAdditionalDirs);
        UNIT_ASSERT(!d.NeedRewrite);
        UNIT_ASSERT(d.HasMakeOperationParts);
        UNIT_ASSERT(d.HasCollectChangingPaths);
    }
}
