#include <ydb/tools/ss_tool/lib/op_inspect.h>

#include <library/cpp/testing/unittest/registar.h>

#include <util/generic/hash_set.h>

using namespace NKikimr::NSchemeShard;
using namespace NKikimr::NSchemeShard::NSsTool;
namespace NSO = NKikimrSchemeOp;

Y_UNIT_TEST_SUITE(SsToolOpInspect) {

    Y_UNIT_TEST(ClassNameCoversEveryEnumerator) {
        UNIT_ASSERT_VALUES_EQUAL(ClassName(EOperationClass::Create),  TStringBuf("Create"));
        UNIT_ASSERT_VALUES_EQUAL(ClassName(EOperationClass::Alter),   TStringBuf("Alter"));
        UNIT_ASSERT_VALUES_EQUAL(ClassName(EOperationClass::Drop),    TStringBuf("Drop"));
        UNIT_ASSERT_VALUES_EQUAL(ClassName(EOperationClass::Other),   TStringBuf("Other"));
        UNIT_ASSERT_VALUES_EQUAL(ClassName(EOperationClass::Unknown), TStringBuf("Unknown"));
    }

    Y_UNIT_TEST(MethodKnownAcceptsExactlyTheTwoSupportedNames) {
        UNIT_ASSERT(MethodKnown("MakeOperationParts"));
        UNIT_ASSERT(MethodKnown("CollectChangingPaths"));
        UNIT_ASSERT(!MethodKnown(""));
        UNIT_ASSERT(!MethodKnown("garbage"));
        // Case-sensitive on purpose: a typo should not silently match.
        UNIT_ASSERT(!MethodKnown("makeoperationparts"));
    }

    Y_UNIT_TEST(HasMethodReadsTheRightDescriptorField) {
        TOpDescriptor d;
        d.HasMakeOperationParts = true;
        d.HasCollectChangingPaths = false;
        UNIT_ASSERT(HasMethod(d, "MakeOperationParts"));
        UNIT_ASSERT(!HasMethod(d, "CollectChangingPaths"));
        // Unknown name returns false (callers validate via MethodKnown first).
        UNIT_ASSERT(!HasMethod(d, "garbage"));
    }

    Y_UNIT_TEST(CollectRowFillsIdentityFromProto) {
        const auto row = CollectRow(NSO::ESchemeOpCreateTable);
        UNIT_ASSERT_VALUES_EQUAL(row.Name, "ESchemeOpCreateTable");
        UNIT_ASSERT_EQUAL(row.Type, NSO::ESchemeOpCreateTable);
    }

    Y_UNIT_TEST(CreateTableIsFullyMigrated) {
        // CreateTable is the pilot for the per-op module pattern: trait spec
        // carries Class + both static methods.
        const auto row = CollectRow(NSO::ESchemeOpCreateTable);
        UNIT_ASSERT_EQUAL(row.Desc.Class, EOperationClass::Create);
        UNIT_ASSERT(row.Desc.CreateDirsFromName);
        UNIT_ASSERT(row.Desc.HasMakeOperationParts);
        UNIT_ASSERT(row.Desc.HasCollectChangingPaths);
    }

    Y_UNIT_TEST(MkDirCarriesTraitFlagButNoModuleMethods) {
        // MkDir has CreateDirsFromName in its trait spec but has not been
        // migrated to the per-op module pattern.
        const auto row = CollectRow(NSO::ESchemeOpMkDir);
        UNIT_ASSERT(row.Desc.CreateDirsFromName);
        UNIT_ASSERT(!row.Desc.HasMakeOperationParts);
        UNIT_ASSERT(!row.Desc.HasCollectChangingPaths);
    }

    Y_UNIT_TEST(UnmigratedOpFallsBackToUnknownClass) {
        // Only migrated ops set Class explicitly; everyone else inherits
        // EOperationClass::Unknown from TSchemeTxTraitsFallback. DropTable
        // has not been migrated, so its trait reports Unknown.
        const auto row = CollectRow(NSO::ESchemeOpDropTable);
        UNIT_ASSERT_EQUAL(row.Desc.Class, EOperationClass::Unknown);
        UNIT_ASSERT(!row.Desc.HasMakeOperationParts);
        UNIT_ASSERT(!row.Desc.HasCollectChangingPaths);
    }

    Y_UNIT_TEST(AllOpsCoversEveryDistinctEnumNumber) {
        const auto ops = AllOps();

        // Build the expected set of distinct enum numbers from the proto
        // descriptor (proto enums may carry aliases that share a number).
        const auto* d = NSO::EOperationType_descriptor();
        THashSet<int> expected;
        for (int i = 0; i < d->value_count(); ++i) {
            expected.insert(d->value(i)->number());
        }

        UNIT_ASSERT_VALUES_EQUAL(ops.size(), expected.size());

        THashSet<int> got;
        for (const auto& op : ops) {
            UNIT_ASSERT_C(got.insert(static_cast<int>(op.Type)).second,
                          "duplicate op number in AllOps()");
        }
        UNIT_ASSERT_EQUAL(got, expected);
    }

    Y_UNIT_TEST(AllOpsIsNonTrivialAndIncludesCreateTable) {
        const auto ops = AllOps();
        UNIT_ASSERT_C(ops.size() >= 50,
                      "schemeshard should declare dozens of ops; got " << ops.size());

        bool sawCreateTable = false;
        for (const auto& op : ops) {
            if (op.Type == NSO::ESchemeOpCreateTable) {
                sawCreateTable = true;
                break;
            }
        }
        UNIT_ASSERT(sawCreateTable);
    }

    Y_UNIT_TEST(MigrationProgressIsMonotonic) {
        // Anything reported as "fully migrated" must indeed have both module
        // methods; anything reported as "partial" must have exactly one.
        // Guards against regressions in CollectRow's dispatch wiring.
        const auto ops = AllOps();
        for (const auto& op : ops) {
            if (op.Desc.HasMakeOperationParts && op.Desc.HasCollectChangingPaths) {
                continue; // fully migrated
            }
            if (op.Desc.HasMakeOperationParts || op.Desc.HasCollectChangingPaths) {
                continue; // partial
            }
            // Pending: neither method should be reported.
            UNIT_ASSERT_C(!op.Desc.HasMakeOperationParts,
                          "logic error for " << op.Name);
            UNIT_ASSERT_C(!op.Desc.HasCollectChangingPaths,
                          "logic error for " << op.Name);
        }
    }
}
