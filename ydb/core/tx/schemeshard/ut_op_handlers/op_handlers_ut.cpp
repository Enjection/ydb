#include <ydb/core/tx/schemeshard/generated/op_handlers.h>
#include <ydb/core/tx/schemeshard/schemeshard_audit_log_fragment.h>

#include <ydb/core/protos/flat_scheme_op.pb.h>

#include <library/cpp/testing/unittest/registar.h>

using namespace NKikimr::NSchemeShard;
namespace NSO = NKikimrSchemeOp;
namespace NHandlers = NKikimr::NSchemeShard::NGenerated::NOpHandlers;

static_assert(NHandlers::IsRegistered_v<NSO::ESchemeOpCreateTable>);
static_assert(!NHandlers::IsRegistered_v<NSO::ESchemeOpDropTable>);
static_assert(!NHandlers::IsRegistered_v<NSO::ESchemeOpMkDir>);

Y_UNIT_TEST_SUITE(SchemeshardOpHandlers) {

    Y_UNIT_TEST(TryCollectChangingPathsRoutesMigratedOp) {
        NSO::TModifyScheme tx;
        tx.SetOperationType(NSO::ESchemeOpCreateTable);
        tx.SetWorkingDir("/Root/Db");
        tx.MutableCreateTable()->SetName("Users");

        auto handled = NHandlers::TryCollectChangingPaths(tx);
        UNIT_ASSERT(handled.has_value());
        UNIT_ASSERT_VALUES_EQUAL(handled->size(), 1u);
        UNIT_ASSERT_VALUES_EQUAL((*handled)[0], "/Root/Db/Users");
    }

    Y_UNIT_TEST(TryCollectChangingPathsReturnsNulloptForUnmigratedOp) {
        NSO::TModifyScheme tx;
        tx.SetOperationType(NSO::ESchemeOpMkDir);
        tx.SetWorkingDir("/Root");
        tx.MutableMkDir()->SetName("NewDir");

        auto handled = NHandlers::TryCollectChangingPaths(tx);
        UNIT_ASSERT(!handled.has_value());
    }

    Y_UNIT_TEST(MakeAuditLogFragmentRoutesCreateTableThroughHandler) {
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
}
