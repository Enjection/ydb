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

    Y_UNIT_TEST(MethodKnownIsCaseSensitive) {
        UNIT_ASSERT(MethodKnown("MakeOperationParts"));
        UNIT_ASSERT(MethodKnown("CollectChangingPaths"));
        UNIT_ASSERT(!MethodKnown(""));
        UNIT_ASSERT(!MethodKnown("garbage"));
        UNIT_ASSERT(!MethodKnown("makeoperationparts"));
    }

    Y_UNIT_TEST(HasMethodReadsTheRightDescriptorField) {
        TOpDescriptor d;
        d.HasMakeOperationParts = true;
        d.HasCollectChangingPaths = false;
        UNIT_ASSERT(HasMethod(d, "MakeOperationParts"));
        UNIT_ASSERT(!HasMethod(d, "CollectChangingPaths"));
        UNIT_ASSERT(!HasMethod(d, "garbage"));
    }

    Y_UNIT_TEST(CollectRowFillsIdentityFromProto) {
        const auto row = CollectRow(NSO::ESchemeOpCreateTable);
        UNIT_ASSERT_VALUES_EQUAL(row.Name, "ESchemeOpCreateTable");
        UNIT_ASSERT_EQUAL(row.Type, NSO::ESchemeOpCreateTable);
    }

    Y_UNIT_TEST(CreateTableIsFullyMigrated) {
        const auto row = CollectRow(NSO::ESchemeOpCreateTable);
        UNIT_ASSERT_EQUAL(row.Desc.Class, EOperationClass::Create);
        UNIT_ASSERT(row.Desc.CreateDirsFromName);
        UNIT_ASSERT(row.Desc.HasMakeOperationParts);
        UNIT_ASSERT(row.Desc.HasCollectChangingPaths);
    }

    Y_UNIT_TEST(MkDirCarriesTraitFlagButNoModuleMethods) {
        const auto row = CollectRow(NSO::ESchemeOpMkDir);
        UNIT_ASSERT(row.Desc.CreateDirsFromName);
        UNIT_ASSERT(!row.Desc.HasMakeOperationParts);
        UNIT_ASSERT(!row.Desc.HasCollectChangingPaths);
    }

    Y_UNIT_TEST(UnmigratedOpFallsBackToUnknownClass) {
        const auto row = CollectRow(NSO::ESchemeOpDropTable);
        UNIT_ASSERT_EQUAL(row.Desc.Class, EOperationClass::Unknown);
        UNIT_ASSERT(!row.Desc.HasMakeOperationParts);
        UNIT_ASSERT(!row.Desc.HasCollectChangingPaths);
    }

    Y_UNIT_TEST(AllOpsCoversEveryDistinctEnumNumber) {
        const auto ops = AllOps();

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
        UNIT_ASSERT_C(ops.size() >= 50, "got " << ops.size());

        bool sawCreateTable = false;
        for (const auto& op : ops) {
            if (op.Type == NSO::ESchemeOpCreateTable) {
                sawCreateTable = true;
                break;
            }
        }
        UNIT_ASSERT(sawCreateTable);
    }
}
