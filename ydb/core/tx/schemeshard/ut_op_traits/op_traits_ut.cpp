#include <ydb/core/tx/schemeshard/schemeshard__dispatch_op.h>
#include <ydb/core/tx/schemeshard/schemeshard__op_traits.h>
#include <ydb/core/tx/schemeshard/schemeshard_audit_log_fragment.h>

#include <ydb/core/protos/flat_scheme_op.pb.h>

#include <library/cpp/testing/unittest/registar.h>

using namespace NKikimr::NSchemeShard;
namespace NSO = NKikimrSchemeOp;

static_assert(TSchemeTxTraitsFallback::Class == EOperationClass::Unknown);
static_assert(TSchemeTxTraits<NSO::ESchemeOpCreateTable>::Class == EOperationClass::Create);
static_assert(Describe<TSchemeTxTraits<NSO::ESchemeOpCreateTable>>().HasMakeOperationParts);
static_assert(Describe<TSchemeTxTraits<NSO::ESchemeOpCreateTable>>().HasCollectChangingPaths);
static_assert(!Describe<TSchemeTxTraits<NSO::ESchemeOpMkDir>>().HasMakeOperationParts);
static_assert(!Describe<TSchemeTxTraits<NSO::ESchemeOpDropTable>>().HasCollectChangingPaths);

Y_UNIT_TEST_SUITE(SchemeshardOpTraits) {

    Y_UNIT_TEST(GetOperationClassReturnsCreateForCreateTable) {
        UNIT_ASSERT_EQUAL(GetOperationClass(NSO::ESchemeOpCreateTable),
                          EOperationClass::Create);
        UNIT_ASSERT(IsCreatePathOperation(NSO::ESchemeOpCreateTable));
    }

    Y_UNIT_TEST(GetOperationClassReturnsDropForDropTable) {
        UNIT_ASSERT_EQUAL(GetOperationClass(NSO::ESchemeOpDropTable),
                          EOperationClass::Drop);
        UNIT_ASSERT(!IsCreatePathOperation(NSO::ESchemeOpDropTable));
    }

    Y_UNIT_TEST(CollectChangingPathsForCreateTableProducesExpectedPath) {
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
        NSO::TModifyScheme tx;
        tx.SetOperationType(NSO::ESchemeOpCreateTable);
        tx.SetWorkingDir("/Root/Db");
        tx.MutableCreateTable()->SetName("Users");

        const auto fragment = MakeAuditLogFragment(tx);
        UNIT_ASSERT_VALUES_EQUAL(fragment.Paths.size(), 1u);
        UNIT_ASSERT_VALUES_EQUAL(fragment.Paths[0], "/Root/Db/Users");
    }

    Y_UNIT_TEST(MakeAuditLogFragmentStillHandlesUnmigratedOps) {
        NSO::TModifyScheme tx;
        tx.SetOperationType(NSO::ESchemeOpMkDir);
        tx.SetWorkingDir("/Root");
        tx.MutableMkDir()->SetName("NewDir");

        const auto fragment = MakeAuditLogFragment(tx);
        UNIT_ASSERT_VALUES_EQUAL(fragment.Paths.size(), 1u);
        UNIT_ASSERT_VALUES_EQUAL(fragment.Paths[0], "/Root/NewDir");
    }

    Y_UNIT_TEST(MakeAuditLogFragmentHandlesCopyFromTableVariant) {
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
        NSO::TModifyScheme tx;
        tx.SetOperationType(NSO::ESchemeOpCreateTable);

        const auto cls = DispatchOp(tx, [](auto traits) {
            return decltype(traits)::Class;
        });
        UNIT_ASSERT_EQUAL(cls, EOperationClass::Create);
    }

    Y_UNIT_TEST(DispatchOpRoutesUnmigratedOpToOwnSpec) {
        NSO::TModifyScheme tx;
        tx.SetOperationType(NSO::ESchemeOpDropTable);

        const auto cls = DispatchOp(tx, [](auto traits) {
            return decltype(traits)::Class;
        });
        UNIT_ASSERT_EQUAL(cls, EOperationClass::Unknown);
    }

    Y_UNIT_TEST(DescribeReportsCreateTableTraitFlags) {
        constexpr auto d = Describe<TSchemeTxTraits<NSO::ESchemeOpCreateTable>>();
        UNIT_ASSERT_EQUAL(d.Class, EOperationClass::Create);
        UNIT_ASSERT(d.CreateDirsFromName);
        UNIT_ASSERT(!d.CreateAdditionalDirs);
        UNIT_ASSERT(!d.NeedRewrite);
        UNIT_ASSERT(d.HasMakeOperationParts);
        UNIT_ASSERT(d.HasCollectChangingPaths);
    }
}
