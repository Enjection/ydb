#include <ydb/core/tx/schemeshard/schemeshard_audit_log_fragment.h>
#include <ydb/core/tx/schemeshard/schemeshard_path_footprint.h>
#include <ydb/core/tx/schemeshard/ut_helpers/helpers.h>

#include <google/protobuf/descriptor.h>

#include <library/cpp/logger/backend.h>
#include <library/cpp/logger/record.h>

#include <util/generic/algorithm.h>
#include <util/generic/hash.h>
#include <util/generic/hash_set.h>
#include <util/string/cast.h>
#include <util/string/join.h>
#include <util/string/split.h>

using namespace NKikimr;
using namespace NKikimr::NSchemeShard;
using namespace NSchemeShardUT_Private;

namespace {

NKikimrSchemeOp::TModifyScheme MakeTx(NKikimrSchemeOp::EOperationType type, const TString& workingDir) {
    NKikimrSchemeOp::TModifyScheme tx;
    tx.SetOperationType(type);
    tx.SetWorkingDir(workingDir);
    return tx;
}

TVector<TString> FieldPaths(const TPathRefs& refs) {
    TVector<TString> result;
    for (const auto& ref : refs) {
        result.push_back(FieldPath(ref));
    }
    return result;
}

void CheckRef(const TPathRef& ref, TStringBuf fieldPath, TStringBuf value,
        EPathRefKind kind, EPathRefRole role)
{
    const TString rendered = FieldPath(ref);
    UNIT_ASSERT_VALUES_EQUAL_C(rendered, TString(fieldPath), "field path");
    UNIT_ASSERT_VALUES_EQUAL_C(TString(ref.Value), TString(value), rendered);
    UNIT_ASSERT_VALUES_EQUAL_C(TString(PathRefKindName(ref.Kind)), TString(PathRefKindName(kind)), rendered);
    UNIT_ASSERT_VALUES_EQUAL_C(TString(PathRefRoleName(ref.Role)), TString(PathRefRoleName(role)), rendered);
}

}  // namespace

Y_UNIT_TEST_SUITE(TSchemeShardPathFootprintExtract) {
    Y_UNIT_TEST(RenderRepeatedAndMapFields) {
        TPathRef ref;
        ref.Field = EPathField::CopyTables_Item_IndexImplDropCdc_StreamName;
        ref.Index = 12;
        ref.SubIndex = 345;
        ref.MapKey = "key{i}";
        UNIT_ASSERT_VALUES_EQUAL(FieldPath(ref),
            "CreateConsistentCopyTables.CopyTableDescriptions[12]"
            ".IndexImplTableDropCdcStreams[key{i}].StreamName[345]");

        ref.Index = 0;
        ref.SubIndex = Max<ui32>();
        ref.MapKey = "";
        UNIT_ASSERT_VALUES_EQUAL(FieldPath(ref),
            "CreateConsistentCopyTables.CopyTableDescriptions[0]"
            ".IndexImplTableDropCdcStreams[].StreamName[4294967295]");

        ref.Field = EPathField::MkDir_Name;
        UNIT_ASSERT_VALUES_EQUAL(FieldPath(ref), "MkDir.Name");
    }

    Y_UNIT_TEST(MkDirAndCreateTable) {
        auto mkdir = MakeTx(NKikimrSchemeOp::ESchemeOpMkDir, "/MyRoot");
        mkdir.MutableMkDir()->SetName("dir");
        auto refs = ExtractPathRefs(mkdir);
        UNIT_ASSERT_VALUES_EQUAL(refs.size(), 1u);
        CheckRef(refs[0], "MkDir.Name", "dir",
            EPathRefKind::LeafUnderWorkingDir, EPathRefRole::Target);

        auto create = MakeTx(NKikimrSchemeOp::ESchemeOpCreateTable, "/MyRoot/dir");
        create.MutableCreateTable()->SetName("Table");
        refs = ExtractPathRefs(create);
        UNIT_ASSERT_VALUES_EQUAL(refs.size(), 1u);
        CheckRef(refs[0], "CreateTable.Name", "Table",
            EPathRefKind::LeafUnderWorkingDir, EPathRefRole::Target);
    }

    Y_UNIT_TEST(DropTableByNameAndById) {
        auto byName = MakeTx(NKikimrSchemeOp::ESchemeOpDropTable, "/MyRoot");
        byName.MutableDrop()->SetName("Table");
        auto refs = ExtractPathRefs(byName);
        UNIT_ASSERT_VALUES_EQUAL(FieldPaths(refs),
            (TVector<TString>{"Drop.Name", "DropTable.<indexes,cdcStreams,implTables>"}));
        CheckRef(refs[0], "Drop.Name", "Table",
            EPathRefKind::LeafUnderWorkingDir, EPathRefRole::Target);
        UNIT_ASSERT_VALUES_EQUAL(TString(PathRefKindName(refs[1].Kind)), "Implicit");
        // the cascade is anchored on the dropped path itself
        UNIT_ASSERT_VALUES_EQUAL(refs[1].AnchorIndex, 0);

        // The id branch is what Propose() actually uses; Name is ignored.
        auto byId = MakeTx(NKikimrSchemeOp::ESchemeOpDropTable, "/MyRoot");
        byId.MutableDrop()->SetName("Table");
        byId.MutableDrop()->SetId(42);
        refs = ExtractPathRefs(byId);
        UNIT_ASSERT_VALUES_EQUAL(FieldPaths(refs),
            (TVector<TString>{"Drop.Id", "DropTable.<indexes,cdcStreams,implTables>"}));
        UNIT_ASSERT_VALUES_EQUAL(TString(PathRefKindName(refs[0].Kind)), "ById");
        UNIT_ASSERT_VALUES_EQUAL(refs[0].LocalPathId, 42u);
        UNIT_ASSERT_VALUES_EQUAL(refs[0].OwnerId, 0u);
    }

    Y_UNIT_TEST(AlterTableById) {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpAlterTable, "/MyRoot");
        tx.MutableAlterTable()->SetName("Table");
        tx.MutableAlterTable()->SetId_Deprecated(7);
        auto refs = ExtractPathRefs(tx);
        UNIT_ASSERT_VALUES_EQUAL(refs.size(), 1u);
        UNIT_ASSERT_VALUES_EQUAL(FieldPath(refs[0]), "AlterTable.Id_Deprecated");
        UNIT_ASSERT_VALUES_EQUAL(refs[0].LocalPathId, 7u);

        TPathId(1234, 9).ToProto(tx.MutableAlterTable()->MutablePathId());
        refs = ExtractPathRefs(tx);
        UNIT_ASSERT_VALUES_EQUAL(refs.size(), 1u);
        UNIT_ASSERT_VALUES_EQUAL(FieldPath(refs[0]), "AlterTable.PathId");
        UNIT_ASSERT_VALUES_EQUAL(refs[0].OwnerId, 1234u);
        UNIT_ASSERT_VALUES_EQUAL(refs[0].LocalPathId, 9u);
    }

    Y_UNIT_TEST(ConsistentCopyTables) {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpCreateConsistentCopyTables, "/MyRoot");
        auto& cfg = *tx.MutableCreateConsistentCopyTables();

        auto& first = *cfg.AddCopyTableDescriptions();
        first.SetSrcPath("/MyRoot/Src0");
        first.SetDstPath("/MyRoot/Dst0");
        first.MutableCreateSrcCdcStream()->MutableStreamDescription()->SetName("srcStream");

        auto& second = *cfg.AddCopyTableDescriptions();
        second.SetSrcPath("/MyRoot/Src1");
        second.SetDstPath("/MyRoot/Dst1");
        (*second.MutableIndexImplTableCdcStreams())["idx/indexImplTable"]
            .MutableStreamDescription()->SetName("implStream");

        const auto refs = ExtractPathRefs(tx);
        UNIT_ASSERT_VALUES_EQUAL(FieldPaths(refs), (TVector<TString>{
            "CreateConsistentCopyTables.CopyTableDescriptions[0].SrcPath",
            "CreateConsistentCopyTables.CopyTableDescriptions[0].DstPath",
            "CreateConsistentCopyTables.CopyTableDescriptions[0].CreateSrcCdcStream.StreamDescription.Name",
            "CreateConsistentCopyTables.CopyTableDescriptions[0].<indexes,implTables,sequences>",
            "CreateConsistentCopyTables.CopyTableDescriptions[1].SrcPath",
            "CreateConsistentCopyTables.CopyTableDescriptions[1].DstPath",
            "CreateConsistentCopyTables.CopyTableDescriptions[1].IndexImplTableCdcStreams[idx/indexImplTable].StreamDescription.Name",
            "CreateConsistentCopyTables.CopyTableDescriptions[1].<indexes,implTables,sequences>",
        }));

        CheckRef(refs[0], "CreateConsistentCopyTables.CopyTableDescriptions[0].SrcPath",
            "/MyRoot/Src0", EPathRefKind::Absolute, EPathRefRole::Source);
        CheckRef(refs[1], "CreateConsistentCopyTables.CopyTableDescriptions[0].DstPath",
            "/MyRoot/Dst0", EPathRefKind::Absolute, EPathRefRole::Target);
        CheckRef(refs[2],
            "CreateConsistentCopyTables.CopyTableDescriptions[0].CreateSrcCdcStream.StreamDescription.Name",
            "srcStream", EPathRefKind::LeafUnderSibling, EPathRefRole::Dependency);
        UNIT_ASSERT_VALUES_EQUAL(refs[2].BasePath, "/MyRoot/Src0");
        UNIT_ASSERT_VALUES_EQUAL(refs[6].BasePath, "/MyRoot/Src1/idx/indexImplTable");
        // per-item cascade is anchored on that item's SrcPath, not the last ref
        UNIT_ASSERT_VALUES_EQUAL(refs[3].AnchorIndex, 0);
        UNIT_ASSERT_VALUES_EQUAL(refs[7].AnchorIndex, 4);
    }

    Y_UNIT_TEST(MoveIndexIsLeafUnderSibling) {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpMoveIndex, "/MyRoot");
        auto& op = *tx.MutableMoveIndex();
        op.SetTablePath("/MyRoot/Table");
        op.SetSrcPath("oldIndex");
        op.SetDstPath("newIndex");

        const auto refs = ExtractPathRefs(tx);
        UNIT_ASSERT_VALUES_EQUAL(FieldPaths(refs), (TVector<TString>{
            "MoveIndex.TablePath", "MoveIndex.SrcPath", "MoveIndex.DstPath",
            "MoveIndex.<indexImplTables>",
        }));
        CheckRef(refs[0], "MoveIndex.TablePath", "/MyRoot/Table",
            EPathRefKind::Absolute, EPathRefRole::Parent);
        CheckRef(refs[1], "MoveIndex.SrcPath", "oldIndex",
            EPathRefKind::LeafUnderSibling, EPathRefRole::Source);
        UNIT_ASSERT_VALUES_EQUAL(refs[1].BasePath, "/MyRoot/Table");
        CheckRef(refs[2], "MoveIndex.DstPath", "newIndex",
            EPathRefKind::LeafUnderSibling, EPathRefRole::Target);
        UNIT_ASSERT_VALUES_EQUAL(refs[3].AnchorIndex, 1);  // anchored on SrcPath
    }

    Y_UNIT_TEST(MoveTableIsAbsolute) {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpMoveTable, "/MyRoot");
        tx.MutableMoveTable()->SetSrcPath("/MyRoot/Src");
        tx.MutableMoveTable()->SetDstPath("/MyRoot/Dst");
        const auto refs = ExtractPathRefs(tx);
        CheckRef(refs[0], "MoveTable.SrcPath", "/MyRoot/Src",
            EPathRefKind::Absolute, EPathRefRole::Source);
        CheckRef(refs[1], "MoveTable.DstPath", "/MyRoot/Dst",
            EPathRefKind::Absolute, EPathRefRole::Target);
    }

    Y_UNIT_TEST(ApplyIndexBuild) {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpApplyIndexBuild, "/MyRoot");
        tx.MutableApplyIndexBuild()->SetTablePath("/MyRoot/Table");
        tx.MutableApplyIndexBuild()->SetIndexName("index");
        const auto refs = ExtractPathRefs(tx);
        UNIT_ASSERT_VALUES_EQUAL(FieldPaths(refs),
            (TVector<TString>{"ApplyIndexBuild.TablePath", "ApplyIndexBuild.IndexName"}));
        CheckRef(refs[1], "ApplyIndexBuild.IndexName", "index",
            EPathRefKind::LeafUnderSibling, EPathRefRole::Target);
        UNIT_ASSERT_VALUES_EQUAL(refs[1].BasePath, "/MyRoot/Table");
    }

    Y_UNIT_TEST(CreateCdcStream) {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpCreateCdcStream, "/MyRoot");
        tx.MutableCreateCdcStream()->SetTableName("Table");
        tx.MutableCreateCdcStream()->MutableStreamDescription()->SetName("Stream");
        const auto refs = ExtractPathRefs(tx);
        UNIT_ASSERT_VALUES_EQUAL(FieldPaths(refs), (TVector<TString>{
            "CreateCdcStream.TableName",
            "CreateCdcStream.StreamDescription.Name",
            "CreateCdcStream.<pqGroupUnderStream>",
        }));
        CheckRef(refs[0], "CreateCdcStream.TableName", "Table",
            EPathRefKind::PathUnderWorkingDir, EPathRefRole::Parent);
        CheckRef(refs[1], "CreateCdcStream.StreamDescription.Name", "Stream",
            EPathRefKind::LeafUnderSibling, EPathRefRole::Target);
        UNIT_ASSERT_VALUES_EQUAL(refs[1].BasePath, "Table");
    }

    // The four *CdcStreamAtTable parts alter the table *and* resolve the stream
    // leaf under it. Reporting only the table loses exactly the path a
    // schema-CDC consumer cares about.

    Y_UNIT_TEST(CreateCdcStreamAtTableReportsTheStreamLeaf) {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpCreateCdcStreamAtTable, "/MyRoot");
        tx.MutableCreateCdcStream()->SetTableName("Table");
        tx.MutableCreateCdcStream()->MutableStreamDescription()->SetName("Stream");
        const auto refs = ExtractPathRefs(tx);
        UNIT_ASSERT_VALUES_EQUAL(FieldPaths(refs), (TVector<TString>{
            "CreateCdcStream.TableName",
            "CreateCdcStream.StreamDescription.Name",
        }));
        // create_cdc_stream.cpp:541 is a plain Child(), so this is a leaf.
        CheckRef(refs[0], "CreateCdcStream.TableName", "Table",
            EPathRefKind::LeafUnderWorkingDir, EPathRefRole::Target);
        CheckRef(refs[1], "CreateCdcStream.StreamDescription.Name", "Stream",
            EPathRefKind::LeafUnderSibling, EPathRefRole::Target);
        UNIT_ASSERT_VALUES_EQUAL(refs[1].BasePath, "Table");
    }

    Y_UNIT_TEST(AlterCdcStreamAtTableReportsTheStreamLeaf) {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpAlterCdcStreamAtTable, "/MyRoot");
        tx.MutableAlterCdcStream()->SetTableName("Table");
        tx.MutableAlterCdcStream()->SetStreamName("Stream");
        const auto refs = ExtractPathRefs(tx);
        UNIT_ASSERT_VALUES_EQUAL(FieldPaths(refs), (TVector<TString>{
            "AlterCdcStream.TableName", "AlterCdcStream.StreamName",
        }));
        CheckRef(refs[1], "AlterCdcStream.StreamName", "Stream",
            EPathRefKind::LeafUnderSibling, EPathRefRole::Target);
        UNIT_ASSERT_VALUES_EQUAL(refs[1].BasePath, "Table");
    }

    Y_UNIT_TEST(DropCdcStreamAtTableReportsEveryStreamLeaf) {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpDropCdcStreamAtTable, "/MyRoot");
        tx.MutableDropCdcStream()->SetTableName("Table");
        tx.MutableDropCdcStream()->AddStreamName("Stream1");
        tx.MutableDropCdcStream()->AddStreamName("Stream2");
        const auto refs = ExtractPathRefs(tx);
        UNIT_ASSERT_VALUES_EQUAL(FieldPaths(refs), (TVector<TString>{
            "DropCdcStream.TableName",
            "DropCdcStream.StreamName[0]",
            "DropCdcStream.StreamName[1]",
        }));
        CheckRef(refs[1], "DropCdcStream.StreamName[0]", "Stream1",
            EPathRefKind::LeafUnderSibling, EPathRefRole::Target);
        CheckRef(refs[2], "DropCdcStream.StreamName[1]", "Stream2",
            EPathRefKind::LeafUnderSibling, EPathRefRole::Target);
        UNIT_ASSERT_VALUES_EQUAL(refs[2].BasePath, "Table");
    }

    Y_UNIT_TEST(RotateCdcStreamAtTableReportsBothStreamLeaves) {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpRotateCdcStreamAtTable, "/MyRoot");
        auto& op = *tx.MutableRotateCdcStream();
        op.SetTableName("Table");
        op.SetOldStreamName("Old");
        op.MutableNewStream()->MutableStreamDescription()->SetName("New");
        const auto refs = ExtractPathRefs(tx);
        UNIT_ASSERT_VALUES_EQUAL(FieldPaths(refs), (TVector<TString>{
            "RotateCdcStream.TableName",
            "RotateCdcStream.OldStreamName",
            "RotateCdcStream.NewStream.StreamDescription.Name",
        }));
        CheckRef(refs[1], "RotateCdcStream.OldStreamName", "Old",
            EPathRefKind::LeafUnderSibling, EPathRefRole::Source);
        CheckRef(refs[2], "RotateCdcStream.NewStream.StreamDescription.Name", "New",
            EPathRefKind::LeafUnderSibling, EPathRefRole::Target);
        UNIT_ASSERT_VALUES_EQUAL(refs[1].BasePath, "Table");
        UNIT_ASSERT_VALUES_EQUAL(refs[2].BasePath, "Table");
    }

    Y_UNIT_TEST(DropTableIndexAtMainTableReportsTheIndexLeaf) {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpDropTableIndexAtMainTable, "/MyRoot");
        tx.MutableDropIndex()->SetTableName("Table");
        tx.MutableDropIndex()->SetIndexName("Index1");
        const auto refs = ExtractPathRefs(tx);
        UNIT_ASSERT_VALUES_EQUAL(FieldPaths(refs), (TVector<TString>{
            "DropIndex.TableName", "DropIndex.IndexName",
        }));
        CheckRef(refs[0], "DropIndex.TableName", "Table",
            EPathRefKind::LeafUnderWorkingDir, EPathRefRole::Target);
        CheckRef(refs[1], "DropIndex.IndexName", "Index1",
            EPathRefKind::LeafUnderSibling, EPathRefRole::Target);
        UNIT_ASSERT_VALUES_EQUAL(refs[1].BasePath, "Table");
    }

    // CreateColumnTable and AlterColumnTable are different TModifyScheme
    // fields. Reading the alter submessage on a create yields one entry with an
    // empty name, which EveryOperationTypeIsCovered cannot distinguish from a
    // real one -- hence the explicit non-empty assertion here.
    Y_UNIT_TEST(CreateColumnTableReadsItsOwnSubmessage) {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpCreateColumnTable, "/MyRoot");
        tx.MutableCreateColumnTable()->SetName("ColumnTable");
        tx.MutableAlterColumnTable()->SetName("WrongOne");
        const auto refs = ExtractPathRefs(tx);
        UNIT_ASSERT_VALUES_EQUAL(refs.size(), 1u);
        CheckRef(refs[0], "CreateColumnTable.Name", "ColumnTable",
            EPathRefKind::LeafUnderWorkingDir, EPathRefRole::Target);
        UNIT_ASSERT(!refs[0].Value.empty());

        auto alter = MakeTx(NKikimrSchemeOp::ESchemeOpAlterColumnTable, "/MyRoot");
        alter.MutableAlterColumnTable()->SetName("ColumnTable");
        const auto alterRefs = ExtractPathRefs(alter);
        // The name, plus the Implicit marker for the tier references the alter
        // drops (olap/operations/alter/common/update.cpp:20).
        UNIT_ASSERT_VALUES_EQUAL(alterRefs.size(), 2u);
        CheckRef(alterRefs[0], "AlterColumnTable.Name", "ColumnTable",
            EPathRefKind::LeafUnderWorkingDir, EPathRefRole::Target);
        CheckRef(alterRefs[1], "AlterColumnTable.<droppedTierStorages>", "",
            EPathRefKind::Implicit, EPathRefRole::Dependency);

        // olap/operations/alter_table.cpp:278: without AlterColumnTable the
        // name comes from AlterTable.Name.
        auto alterViaTable = MakeTx(NKikimrSchemeOp::ESchemeOpAlterColumnTable, "/MyRoot");
        alterViaTable.MutableAlterTable()->SetName("ColumnTable");
        const auto fallbackRefs = ExtractPathRefs(alterViaTable);
        UNIT_ASSERT_VALUES_EQUAL(fallbackRefs.size(), 1u);
        CheckRef(fallbackRefs[0], "AlterTable.Name", "ColumnTable",
            EPathRefKind::LeafUnderWorkingDir, EPathRefRole::Target);
    }

    // Only two of the four AtTable parts split a multi-segment TableName, and
    // the extractor has to match each one rather than pick a single rule:
    //
    //   create_cdc_stream.cpp:541  workingDirPath.Child(tableName)          plain
    //   alter_cdc_stream.cpp:375   .Child(tableName, TSplitChildTag{})      split
    //   drop_cdc_stream.cpp:361    TPath::Resolve(workingDir).Dive(name)    plain
    //   rotate_cdc_stream.cpp:543  .Child(tableName, TSplitChildTag{})      split
    //
    // Plain Dive() pushes "a/b" into NameParts verbatim, so it looks up a child
    // literally named "a/b" and does not resolve. The PathString is the same
    // either way; Exists, PathId and LeafName are not.
    Y_UNIT_TEST(OnlyAlterAndRotateAtTableSplitTheTableName) {
        const TString multi = "Table/Index/indexImplTable";

        auto create = MakeTx(NKikimrSchemeOp::ESchemeOpCreateCdcStreamAtTable, "/MyRoot");
        create.MutableCreateCdcStream()->SetTableName(multi);
        create.MutableCreateCdcStream()->MutableStreamDescription()->SetName("Stream");
        auto refs = ExtractPathRefs(create);
        UNIT_ASSERT_VALUES_EQUAL(refs.size(), 2u);
        CheckRef(refs[0], "CreateCdcStream.TableName", multi,
            EPathRefKind::LeafUnderWorkingDir, EPathRefRole::Target);

        auto drop = MakeTx(NKikimrSchemeOp::ESchemeOpDropCdcStreamAtTable, "/MyRoot");
        drop.MutableDropCdcStream()->SetTableName(multi);
        drop.MutableDropCdcStream()->AddStreamName("Stream");
        refs = ExtractPathRefs(drop);
        CheckRef(refs[0], "DropCdcStream.TableName", multi,
            EPathRefKind::LeafUnderWorkingDir, EPathRefRole::Target);

        auto alter = MakeTx(NKikimrSchemeOp::ESchemeOpAlterCdcStreamAtTable, "/MyRoot");
        alter.MutableAlterCdcStream()->SetTableName(multi);
        alter.MutableAlterCdcStream()->SetStreamName("Stream");
        refs = ExtractPathRefs(alter);
        UNIT_ASSERT_VALUES_EQUAL(refs.size(), 2u);
        CheckRef(refs[0], "AlterCdcStream.TableName", multi,
            EPathRefKind::PathUnderWorkingDirSplit, EPathRefRole::Target);
        UNIT_ASSERT_VALUES_EQUAL(refs[1].BasePath, multi);

        auto rotate = MakeTx(NKikimrSchemeOp::ESchemeOpRotateCdcStreamAtTable, "/MyRoot");
        rotate.MutableRotateCdcStream()->SetTableName(multi);
        rotate.MutableRotateCdcStream()->SetOldStreamName("Old");
        rotate.MutableRotateCdcStream()->MutableNewStream()
            ->MutableStreamDescription()->SetName("New");
        refs = ExtractPathRefs(rotate);
        UNIT_ASSERT_VALUES_EQUAL(refs.size(), 3u);
        CheckRef(refs[0], "RotateCdcStream.TableName", multi,
            EPathRefKind::PathUnderWorkingDirSplit, EPathRefRole::Target);
    }

    // Propose() -> RegisterBackupCollectionTables() resolves every entry as an
    // absolute path, so the collection's members are part of the footprint.
    Y_UNIT_TEST(CreateBackupCollectionReportsExplicitEntries) {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpCreateBackupCollection,
            "/MyRoot/.backups/collections");
        auto& op = *tx.MutableCreateBackupCollection();
        op.SetName("coll");
        op.MutableExplicitEntryList()->AddEntries()->SetPath("/MyRoot/Table1");
        op.MutableExplicitEntryList()->AddEntries()->SetPath("/MyRoot/dir/Table2");
        const auto refs = ExtractPathRefs(tx);
        UNIT_ASSERT_VALUES_EQUAL(FieldPaths(refs), (TVector<TString>{
            "CreateBackupCollection.Name",
            "CreateBackupCollection.ExplicitEntryList.Entries[0].Path",
            "CreateBackupCollection.ExplicitEntryList.Entries[1].Path",
        }));
        CheckRef(refs[1], "CreateBackupCollection.ExplicitEntryList.Entries[0].Path",
            "/MyRoot/Table1", EPathRefKind::Absolute, EPathRefRole::Dependency);
        CheckRef(refs[2], "CreateBackupCollection.ExplicitEntryList.Entries[1].Path",
            "/MyRoot/dir/Table2", EPathRefKind::Absolute, EPathRefRole::Dependency);
    }

    // MoveTable and MoveIndex both mark their cascade; MoveTableIndex must too,
    // otherwise its impl tables and sequences are a silent gap.
    Y_UNIT_TEST(MoveTableIndexMarksItsCascade) {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpMoveTableIndex, "/MyRoot");
        tx.MutableMoveTableIndex()->SetSrcPath("/MyRoot/Table/idx");
        tx.MutableMoveTableIndex()->SetDstPath("/MyRoot/Moved/idx");
        const auto refs = ExtractPathRefs(tx);
        UNIT_ASSERT_VALUES_EQUAL(FieldPaths(refs), (TVector<TString>{
            "MoveTableIndex.SrcPath", "MoveTableIndex.DstPath",
            "MoveTableIndex.<indexImplTables,sequences>",
        }));
        CheckRef(refs[0], "MoveTableIndex.SrcPath", "/MyRoot/Table/idx",
            EPathRefKind::Absolute, EPathRefRole::Source);
        UNIT_ASSERT_VALUES_EQUAL(TString(PathRefKindName(refs[2].Kind)), "Implicit");
        UNIT_ASSERT_VALUES_EQUAL(refs[2].AnchorIndex, 0);  // anchored on SrcPath
    }

    Y_UNIT_TEST(CreateIndexedTable) {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpCreateIndexedTable, "/MyRoot");
        auto& cfg = *tx.MutableCreateIndexedTable();
        cfg.MutableTableDescription()->SetName("Table");
        cfg.AddIndexDescription()->SetName("byValue");
        const auto refs = ExtractPathRefs(tx);
        UNIT_ASSERT_VALUES_EQUAL(FieldPaths(refs), (TVector<TString>{
            "CreateIndexedTable.TableDescription.Name",
            "CreateIndexedTable.IndexDescription[0].Name",
            "CreateIndexedTable.<indexImplTables>",
        }));
        CheckRef(refs[1], "CreateIndexedTable.IndexDescription[0].Name", "byValue",
            EPathRefKind::LeafUnderSibling, EPathRefRole::Dependency);
        UNIT_ASSERT_VALUES_EQUAL(refs[1].BasePath, "Table");
    }

    Y_UNIT_TEST(AlterUserAttributesTakesAbsolutePath) {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpAlterUserAttributes, "/MyRoot");
        tx.MutableAlterUserAttributes()->SetPathName("/MyRoot/dir/sub");
        const auto refs = ExtractPathRefs(tx);
        UNIT_ASSERT_VALUES_EQUAL(refs.size(), 1u);
        CheckRef(refs[0], "AlterUserAttributes.PathName", "/MyRoot/dir/sub",
            EPathRefKind::PathUnderWorkingDir, EPathRefRole::Target);
    }

    Y_UNIT_TEST(AlterLoginTouchesNoPath) {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpAlterLogin, "/MyRoot");
        tx.MutableAlterLogin()->MutableCreateUser()->SetUser("user1");
        UNIT_ASSERT_VALUES_EQUAL(ExtractPathRefs(tx).size(), 0u);
    }

    // Removing a sid is the one AlterLogin shape that reads paths: CanRemoveSid
    // scans the database subtree for an owner or ACL record.
    Y_UNIT_TEST(AlterLoginRemoveScansTheDatabaseSubtree) {
        for (const bool group : {false, true}) {
            auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpAlterLogin, "/MyRoot");
            if (group) {
                tx.MutableAlterLogin()->MutableRemoveGroup()->SetGroup("g1");
            } else {
                tx.MutableAlterLogin()->MutableRemoveUser()->SetUser("user1");
            }
            const auto refs = ExtractPathRefs(tx);
            UNIT_ASSERT_VALUES_EQUAL(refs.size(), 2u);
            CheckRef(refs[0], "<WorkingDir>", "",
                EPathRefKind::PathUnderWorkingDir, EPathRefRole::Target);
            CheckRef(refs[1], "AlterLogin.<aclScanSubtree>", "",
                EPathRefKind::Implicit, EPathRefRole::Dependency);
        }
    }

    // CheckApplyIf resolves every precondition path id before the operation
    // itself runs, for any operation type (found by the read-set gate in
    // ut_user_attributes).
    Y_UNIT_TEST(ApplyIfPathIdsAreDependencies) {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpMkDir, "/MyRoot");
        tx.MutableMkDir()->SetName("DirB");
        tx.AddApplyIf()->SetPathId(5);
        tx.AddApplyIf()->SetPathId(7);
        const auto refs = ExtractPathRefs(tx);
        UNIT_ASSERT_VALUES_EQUAL(FieldPaths(refs), (TVector<TString>{
            "MkDir.Name", "ApplyIf[0].PathId", "ApplyIf[1].PathId",
        }));
        CheckRef(refs[1], "ApplyIf[0].PathId", "",
            EPathRefKind::ById, EPathRefRole::Dependency);
        UNIT_ASSERT_VALUES_EQUAL(refs[1].LocalPathId, 5u);
        UNIT_ASSERT_VALUES_EQUAL(refs[2].LocalPathId, 7u);
    }

    Y_UNIT_TEST(CreateFullBackupOpTargetsWorkingDir) {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpCreateFullBackupOp,
            "/MyRoot/.backups/collections/coll");
        const auto refs = ExtractPathRefs(tx);
        UNIT_ASSERT_VALUES_EQUAL(FieldPaths(refs), (TVector<TString>{
            "<WorkingDir>", "CreateFullBackupOp.<collectionEntries>",
        }));
        CheckRef(refs[0], "<WorkingDir>", "",
            EPathRefKind::PathUnderWorkingDir, EPathRefRole::Target);
    }

    Y_UNIT_TEST(SplitMergeIsAbsoluteNotWorkingDirRelative) {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpSplitMergeTablePartitions, "/MyRoot");
        tx.MutableSplitMergeTablePartitions()->SetTablePath("/MyRoot/Table");
        auto refs = ExtractPathRefs(tx);
        CheckRef(refs[0], "SplitMergeTablePartitions.TablePath", "/MyRoot/Table",
            EPathRefKind::Absolute, EPathRefRole::Target);

        tx.MutableSplitMergeTablePartitions()->SetTableOwnerId(72057594046678944ull);
        tx.MutableSplitMergeTablePartitions()->SetTableLocalId(3);
        refs = ExtractPathRefs(tx);
        UNIT_ASSERT_VALUES_EQUAL(refs.size(), 1u);
        UNIT_ASSERT_VALUES_EQUAL(FieldPath(refs[0]), "SplitMergeTablePartitions.TableLocalId");
        UNIT_ASSERT_VALUES_EQUAL(refs[0].OwnerId, 72057594046678944ull);
        UNIT_ASSERT_VALUES_EQUAL(refs[0].LocalPathId, 3u);
    }

    // Every EOperationType must be handled. The switch in ExtractPathRefs has
    // no `default:`, so a new enum value is a -Wswitch compile error; this test
    // additionally pins which op types legitimately extract nothing.
    // -- §8.3 fields: paths the request names that the shape audit missed --

    Y_UNIT_TEST(CreateTableCopyFromTableIsAnAbsoluteSource) {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpCreateTable, "/MyRoot/dir");
        tx.MutableCreateTable()->SetName("Dst");
        tx.MutableCreateTable()->SetCopyFromTable("/MyRoot/other/Src");
        const auto refs = ExtractPathRefs(tx);
        UNIT_ASSERT_VALUES_EQUAL(FieldPaths(refs),
            (TVector<TString>{"CreateTable.Name", "CreateTable.CopyFromTable"}));
        CheckRef(refs[1], "CreateTable.CopyFromTable", "/MyRoot/other/Src",
            EPathRefKind::Absolute, EPathRefRole::Source);

        // Without the field the op is a plain create and nothing extra appears.
        tx.MutableCreateTable()->ClearCopyFromTable();
        UNIT_ASSERT_VALUES_EQUAL(FieldPaths(ExtractPathRefs(tx)),
            (TVector<TString>{"CreateTable.Name"}));
    }

    Y_UNIT_TEST(CreateColumnTableCopyFromTableIsAnAbsoluteSource) {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpCreateColumnTable, "/MyRoot/dir");
        tx.MutableCreateColumnTable()->SetName("Dst");
        tx.MutableCreateColumnTable()->SetCopyFromTable("/MyRoot/store/Src");
        const auto refs = ExtractPathRefs(tx);
        UNIT_ASSERT_VALUES_EQUAL(FieldPaths(refs),
            (TVector<TString>{"CreateColumnTable.Name", "CreateColumnTable.CopyFromTable"}));
        CheckRef(refs[1], "CreateColumnTable.CopyFromTable", "/MyRoot/store/Src",
            EPathRefKind::Absolute, EPathRefRole::Source);
    }

    Y_UNIT_TEST(CopySequenceCopyFromIsAnAbsoluteSource) {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpCreateSequence, "/MyRoot/Dst");
        tx.MutableSequence()->SetName("myseq");
        tx.MutableCopySequence()->SetCopyFrom("/MyRoot/Src/myseq");
        const auto refs = ExtractPathRefs(tx);
        UNIT_ASSERT_VALUES_EQUAL(FieldPaths(refs),
            (TVector<TString>{"Sequence.Name", "CopySequence.CopyFrom"}));
        CheckRef(refs[1], "CopySequence.CopyFrom", "/MyRoot/Src/myseq",
            EPathRefKind::Absolute, EPathRefRole::Source);
    }

    Y_UNIT_TEST(AlterTableDefaultFromSequence) {
        // Relative form: a leaf under the altered table itself.
        auto byName = MakeTx(NKikimrSchemeOp::ESchemeOpAlterTable, "/MyRoot");
        byName.MutableAlterTable()->SetName("Table");
        byName.MutableAlterTable()->AddColumns()->SetDefaultFromSequence("myseq");
        auto refs = ExtractPathRefs(byName);
        UNIT_ASSERT_VALUES_EQUAL(FieldPaths(refs), (TVector<TString>{
            "AlterTable.Name", "AlterTable.Columns[0].DefaultFromSequence"}));
        CheckRef(refs[1], "AlterTable.Columns[0].DefaultFromSequence", "myseq",
            EPathRefKind::LeafUnderSibling, EPathRefRole::Dependency);
        // The base is the table entry, not a name string: the table may be
        // addressed by path id, where there is no name to join.
        UNIT_ASSERT_VALUES_EQUAL(refs[1].BasePath, "");
        UNIT_ASSERT_VALUES_EQUAL(refs[1].AnchorIndex, 0);

        // Absolute form: WorkingDir and the table are both out of the picture.
        auto absolute = MakeTx(NKikimrSchemeOp::ESchemeOpAlterTable, "/MyRoot");
        absolute.MutableAlterTable()->SetName("Table");
        absolute.MutableAlterTable()->AddColumns()->SetName("plain");
        absolute.MutableAlterTable()->AddColumns()->SetDefaultFromSequence("/MyRoot/seq");
        refs = ExtractPathRefs(absolute);
        UNIT_ASSERT_VALUES_EQUAL(FieldPaths(refs), (TVector<TString>{
            "AlterTable.Name", "AlterTable.Columns[1].DefaultFromSequence"}));
        CheckRef(refs[1], "AlterTable.Columns[1].DefaultFromSequence", "/MyRoot/seq",
            EPathRefKind::Absolute, EPathRefRole::Dependency);

        // By path id, the anchor still points at the table entry.
        auto byId = MakeTx(NKikimrSchemeOp::ESchemeOpAlterTable, "/MyRoot");
        TPathId(1, 5).ToProto(byId.MutableAlterTable()->MutablePathId());
        byId.MutableAlterTable()->AddColumns()->SetDefaultFromSequence("myseq");
        refs = ExtractPathRefs(byId);
        UNIT_ASSERT_VALUES_EQUAL(FieldPaths(refs), (TVector<TString>{
            "AlterTable.PathId", "AlterTable.Columns[0].DefaultFromSequence"}));
        UNIT_ASSERT_VALUES_EQUAL(refs[1].AnchorIndex, 0);
    }

    Y_UNIT_TEST(TransferAndReplicationDestinationPaths) {
        // Transfer: both destination fields are resolved by Propose.
        auto transfer = MakeTx(NKikimrSchemeOp::ESchemeOpCreateTransfer, "/MyRoot");
        transfer.MutableReplication()->SetName("transfer");
        auto& target = *transfer.MutableReplication()->MutableConfig()
            ->MutableTransferSpecific()->MutableTarget();
        target.SetSrcPath("/RemoteRoot/topic");
        target.SetDstPath("/MyRoot/Dst");
        target.SetDirectoryPath("/MyRoot/dir");
        auto refs = ExtractPathRefs(transfer);
        UNIT_ASSERT_VALUES_EQUAL(FieldPaths(refs), (TVector<TString>{
            "Replication.Name",
            "Replication.Config.TransferSpecific.Target.DstPath",
            "Replication.Config.TransferSpecific.Target.DirectoryPath"}));
        CheckRef(refs[1], "Replication.Config.TransferSpecific.Target.DstPath",
            "/MyRoot/Dst", EPathRefKind::Absolute, EPathRefRole::Dependency);
        CheckRef(refs[2], "Replication.Config.TransferSpecific.Target.DirectoryPath",
            "/MyRoot/dir", EPathRefKind::Absolute, EPathRefRole::Dependency);

        // Replication: the repeated targets carry local destinations. SrcPath
        // names an object on the *remote* cluster and must never be emitted.
        auto replication = MakeTx(NKikimrSchemeOp::ESchemeOpCreateReplication, "/MyRoot");
        replication.MutableReplication()->SetName("repl");
        auto& specific = *replication.MutableReplication()->MutableConfig()->MutableSpecific();
        auto& first = *specific.AddTargets();
        first.SetSrcPath("/RemoteRoot/Table");
        first.SetDstPath("/MyRoot/Replicated/Table");
        auto& second = *specific.AddTargets();
        second.SetSrcPath("/RemoteRoot/Other");
        second.SetDstPath("/MyRoot/Replicated/Other");
        refs = ExtractPathRefs(replication);
        UNIT_ASSERT_VALUES_EQUAL(FieldPaths(refs), (TVector<TString>{
            "Replication.Name",
            "Replication.Config.Specific.Targets[0].DstPath",
            "Replication.Config.Specific.Targets[1].DstPath"}));
        CheckRef(refs[1], "Replication.Config.Specific.Targets[0].DstPath",
            "/MyRoot/Replicated/Table", EPathRefKind::Absolute, EPathRefRole::Dependency);

        // Alter carries the directory in its own submessage.
        auto alter = MakeTx(NKikimrSchemeOp::ESchemeOpAlterTransfer, "/MyRoot");
        alter.MutableAlterReplication()->SetName("transfer");
        alter.MutableAlterReplication()->MutableAlterTransfer()->SetDirectoryPath("/MyRoot/dir2");
        refs = ExtractPathRefs(alter);
        UNIT_ASSERT_VALUES_EQUAL(FieldPaths(refs), (TVector<TString>{
            "AlterReplication.Name", "AlterReplication.AlterTransfer.DirectoryPath"}));
        CheckRef(refs[1], "AlterReplication.AlterTransfer.DirectoryPath",
            "/MyRoot/dir2", EPathRefKind::Absolute, EPathRefRole::Dependency);
    }

    Y_UNIT_TEST(AlterPersQueueGroupOffloadDstPath) {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpAlterPersQueueGroup, "/MyRoot");
        tx.MutableAlterPersQueueGroup()->SetName("Topic");
        tx.MutableAlterPersQueueGroup()->MutablePQTabletConfig()->MutableOffloadConfig()
            ->MutableIncrementalBackup()->SetDstPath("/MyRoot/backup/Table");
        const auto refs = ExtractPathRefs(tx);
        UNIT_ASSERT_VALUES_EQUAL(FieldPaths(refs), (TVector<TString>{
            "AlterPersQueueGroup.Name",
            "AlterPersQueueGroup.PQTabletConfig.OffloadConfig.IncrementalBackup.DstPath"}));
        CheckRef(refs[1],
            "AlterPersQueueGroup.PQTabletConfig.OffloadConfig.IncrementalBackup.DstPath",
            "/MyRoot/backup/Table", EPathRefKind::Absolute, EPathRefRole::Dependency);

        // The other offload strategy names no path.
        tx.MutableAlterPersQueueGroup()->MutablePQTabletConfig()->MutableOffloadConfig()
            ->MutableIncrementalRestore();
        UNIT_ASSERT_VALUES_EQUAL(FieldPaths(ExtractPathRefs(tx)),
            (TVector<TString>{"AlterPersQueueGroup.Name"}));
    }

    Y_UNIT_TEST(AlterContinuousBackupTakeIncrementalBackup) {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpAlterContinuousBackup, "/MyRoot");
        tx.MutableAlterContinuousBackup()->SetTableName("dir/Table");
        auto& take = *tx.MutableAlterContinuousBackup()->MutableTakeIncrementalBackup();
        take.SetDstPath("backups/Table_incr");
        take.SetDstStreamPath("newStream");
        const auto refs = ExtractPathRefs(tx);
        UNIT_ASSERT_VALUES_EQUAL(FieldPaths(refs), (TVector<TString>{
            "AlterContinuousBackup.TableName",
            "AlterContinuousBackup.TakeIncrementalBackup.DstPath",
            "AlterContinuousBackup.TakeIncrementalBackup.DstStreamPath",
            "AlterContinuousBackup.<incrementalBackupTable>"}));
        // Both are Child(..., TSplitChildTag{}) under WorkingDir.
        CheckRef(refs[0], "AlterContinuousBackup.TableName", "dir/Table",
            EPathRefKind::PathUnderWorkingDirSplit, EPathRefRole::Target);
        CheckRef(refs[1], "AlterContinuousBackup.TakeIncrementalBackup.DstPath",
            "backups/Table_incr", EPathRefKind::PathUnderWorkingDirSplit, EPathRefRole::Target);
        CheckRef(refs[2], "AlterContinuousBackup.TakeIncrementalBackup.DstStreamPath",
            "newStream", EPathRefKind::LeafUnderSibling, EPathRefRole::Target);
        UNIT_ASSERT_VALUES_EQUAL(refs[2].AnchorIndex, 0);
        UNIT_ASSERT_VALUES_EQUAL(refs[3].AnchorIndex, 0);

        // Stop names no destination at all.
        auto stop = MakeTx(NKikimrSchemeOp::ESchemeOpAlterContinuousBackup, "/MyRoot");
        stop.MutableAlterContinuousBackup()->SetTableName("Table");
        stop.MutableAlterContinuousBackup()->MutableStop();
        UNIT_ASSERT_VALUES_EQUAL(FieldPaths(ExtractPathRefs(stop)), (TVector<TString>{
            "AlterContinuousBackup.TableName",
            "AlterContinuousBackup.<incrementalBackupTable>"}));
    }

    Y_UNIT_TEST(CreateContinuousBackupStreamName) {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpCreateContinuousBackup, "/MyRoot");
        tx.MutableCreateContinuousBackup()->SetTableName("Table");
        tx.MutableCreateContinuousBackup()->MutableContinuousBackupDescription()
            ->SetStreamName("stream");
        auto refs = ExtractPathRefs(tx);
        UNIT_ASSERT_VALUES_EQUAL(FieldPaths(refs), (TVector<TString>{
            "CreateContinuousBackup.TableName",
            "CreateContinuousBackup.ContinuousBackupDescription.StreamName",
            "CreateContinuousBackup.<cdcStream>"}));
        CheckRef(refs[1], "CreateContinuousBackup.ContinuousBackupDescription.StreamName",
            "stream", EPathRefKind::LeafUnderSibling, EPathRefRole::Target);
        UNIT_ASSERT_VALUES_EQUAL(refs[1].AnchorIndex, 0);

        // Without the field the stream name is generated from the current time,
        // so only the Implicit marker stands for it.
        tx.MutableCreateContinuousBackup()->MutableContinuousBackupDescription()
            ->ClearStreamName();
        UNIT_ASSERT_VALUES_EQUAL(FieldPaths(ExtractPathRefs(tx)), (TVector<TString>{
            "CreateContinuousBackup.TableName", "CreateContinuousBackup.<cdcStream>"}));
    }

    Y_UNIT_TEST(EveryOperationTypeIsCovered) {
        const THashSet<TString> noPathOps = {
            "ESchemeOp_DEPRECATED_35",
            "ESchemeOpAlterLogin",
            "ESchemeOpAlterBlobDepot",
            "ESchemeOpDropBlobDepot",
            "ESchemeOpAlterView",
            "ESchemeOpIncrementalRestoreLockTargets",
            "ESchemeOpIncrementalRestoreUnlockTargets",
            // repeated-only requests: nothing to extract from an empty proto
            "ESchemeOpCreateConsistentCopyTables",
        };

        const auto* descriptor = NKikimrSchemeOp::EOperationType_descriptor();
        UNIT_ASSERT(descriptor);
        UNIT_ASSERT(descriptor->value_count() > 0);

        for (int i = 0; i < descriptor->value_count(); ++i) {
            const auto* value = descriptor->value(i);
            const TString name(value->name());
            auto tx = MakeTx(static_cast<NKikimrSchemeOp::EOperationType>(value->number()), "/MyRoot");
            const auto refs = ExtractPathRefs(tx);
            if (noPathOps.contains(name)) {
                UNIT_ASSERT_VALUES_EQUAL_C(refs.size(), 0u, name);
            } else {
                UNIT_ASSERT_C(!refs.empty(), "no path refs extracted for " << name);
            }
        }
    }

    // EveryOperationTypeIsCovered above can only see that *something* was
    // extracted: an op that reads the wrong submessage still yields one entry,
    // with an empty name. This catches that class -- a Create* op reading an
    // Alter* submessage, or vice versa -- which is exactly how the extractor
    // read AlterColumnTable.Name on ESchemeOpCreateColumnTable.
    Y_UNIT_TEST(OpVerbMatchesTheSubmessageItReads) {
        // Real, checked-by-hand cross-verb reads: these op types genuinely
        // carry their payload in the other submessage.
        const THashSet<TString> intentional = {
            "ESchemeOpAlterExternalTable/CreateExternalTable",
            "ESchemeOpAlterExternalDataSource/CreateExternalDataSource",
            "ESchemeOpAlterResourcePool/CreateResourcePool",
            "ESchemeOpAlterStreamingQuery/CreateStreamingQuery",
        };
        const TVector<TString> verbs = {"Create", "Alter", "Drop", "Move", "Rotate"};

        const auto verbOf = [&](TStringBuf s) -> TString {
            for (const auto& v : verbs) {
                if (s.StartsWith(v)) {
                    return v;
                }
            }
            return TString();
        };

        const auto* descriptor = NKikimrSchemeOp::EOperationType_descriptor();
        UNIT_ASSERT(descriptor);

        for (int i = 0; i < descriptor->value_count(); ++i) {
            const TString name(descriptor->value(i)->name());
            auto tx = MakeTx(static_cast<NKikimrSchemeOp::EOperationType>(
                descriptor->value(i)->number()), "/MyRoot");
            TStringBuf shortName(name);
            shortName.SkipPrefix("ESchemeOp");
            const TString opVerb = verbOf(shortName);
            if (opVerb.empty()) {
                continue;
            }
            for (const auto& ref : ExtractPathRefs(tx)) {
                // The submessage this ref reads is the FieldPath's first
                // segment: "AlterCdcStream.StreamName" -> "AlterCdcStream".
                const TString refFieldPath = FieldPath(ref);
                TString submessage = refFieldPath;
                const size_t cut = submessage.find_first_of(".[");
                if (cut != TString::npos) {
                    submessage = submessage.substr(0, cut);
                }
                if (submessage.StartsWith('<') || submessage == "Drop") {
                    continue;  // marker, or the generic TDrop submessage
                }
                const TString refVerb = verbOf(submessage);
                if (refVerb.empty() || refVerb == opVerb) {
                    continue;
                }
                UNIT_ASSERT_C(intentional.contains(name + "/" + submessage),
                    name << " extracts from " << submessage
                         << " (field path " << refFieldPath << "): a " << opVerb
                         << "* operation reading an " << refVerb
                         << "* submessage is almost always a copy-paste bug."
                         << " If it is deliberate, add it to `intentional`.");
            }
        }
    }

    // The field table is the single source of truth for field identity: the
    // enum, the rendered field paths and KnownPathFieldNames() all come from
    // SCHEMESHARD_PATH_FIELDS. This pins the properties the rest of the suite
    // and the descriptor walk rely on.
    Y_UNIT_TEST(EveryPathFieldRendersAndIsListedOnce) {
        const size_t count = static_cast<size_t>(EPathField::Count);
        UNIT_ASSERT_C(count > 100, "the field table has only " << count << " rows");

        THashSet<TString> templates;
        THashSet<TString> protoNames;
        size_t synthetic = 0;
        for (size_t i = 0; i < count; ++i) {
            const auto field = static_cast<EPathField>(i);
            const TString tmpl(PathFieldName(field));
            UNIT_ASSERT_C(!tmpl.empty(), "field " << i << " has no field-path template");
            UNIT_ASSERT_C(templates.insert(tmpl).second,
                "two path fields share the field-path template " << tmpl);

            TPathRef ref;
            ref.Field = field;
            ref.Index = 3;
            ref.SubIndex = 7;
            ref.MapKey = "someKey";
            const TString rendered = FieldPath(ref);
            UNIT_ASSERT_C(rendered.find('{') == TString::npos
                    && rendered.find('}') == TString::npos,
                "unexpanded placeholder in " << rendered);
            if (tmpl.Contains("{i}")) {
                UNIT_ASSERT_C(rendered.Contains("[3]"), rendered);
            }
            if (tmpl.Contains("{j}")) {
                UNIT_ASSERT_C(rendered.Contains("[7]"), rendered);
            }
            if (tmpl.Contains("{key}")) {
                UNIT_ASSERT_C(rendered.Contains("[someKey]"), rendered);
            }
            if (tmpl.find('{') == TString::npos) {
                UNIT_ASSERT_VALUES_EQUAL(rendered, tmpl);
            }

            const TString proto(PathFieldProtoName(field));
            if (proto.empty()) {
                ++synthetic;
            } else {
                protoNames.insert(proto);
            }
        }
        UNIT_ASSERT_C(synthetic > 0, "no synthetic (marker or id) field rows");

        const auto& known = KnownPathFieldNames();
        THashSet<TString> knownSet;
        for (const TStringBuf name : known) {
            UNIT_ASSERT_C(!name.empty(), "KnownPathFieldNames() has an empty entry");
            UNIT_ASSERT_C(knownSet.insert(TString(name)).second,
                "KnownPathFieldNames() lists " << name << " twice");
        }
        UNIT_ASSERT_VALUES_EQUAL(known.size(), protoNames.size());
        for (const auto& name : protoNames) {
            UNIT_ASSERT_C(knownSet.contains(name),
                name << " is in the field table but not in KnownPathFieldNames()");
        }
        UNIT_ASSERT_C(IsSorted(known.begin(), known.end()),
            "KnownPathFieldNames() is not sorted");
    }

    // Extraction reads the request, it does not copy it: every value is a view
    // into the TModifyScheme that was passed in. Only the resolve step, which
    // has to outlive the request, materializes strings.
    Y_UNIT_TEST(ExtractedValuesPointIntoTheRequest) {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpMoveTable, "/MyRoot");
        tx.MutableMoveTable()->SetSrcPath("/MyRoot/Src");
        tx.MutableMoveTable()->SetDstPath("/MyRoot/Dst");

        const auto refs = ExtractPathRefs(tx);
        UNIT_ASSERT_VALUES_EQUAL(refs.size(), 3u);
        UNIT_ASSERT_EQUAL(refs[0].Value.data(), tx.GetMoveTable().GetSrcPath().data());
        UNIT_ASSERT_EQUAL(refs[1].Value.data(), tx.GetMoveTable().GetDstPath().data());

        // A sibling base is a view too, when the request spells it out.
        auto move = MakeTx(NKikimrSchemeOp::ESchemeOpMoveIndex, "/MyRoot");
        move.MutableMoveIndex()->SetTablePath("/MyRoot/Table");
        move.MutableMoveIndex()->SetSrcPath("oldIndex");
        const auto moveRefs = ExtractPathRefs(move);
        UNIT_ASSERT_EQUAL(moveRefs[1].Value.data(), move.GetMoveIndex().GetSrcPath().data());
        UNIT_ASSERT_EQUAL(moveRefs[1].BasePath.data(), move.GetMoveIndex().GetTablePath().data());
    }
}

Y_UNIT_TEST_SUITE(TSchemeShardAuditLogPaths) {

TString AuditPaths(const NKikimrSchemeOp::TModifyScheme& tx) {
    return JoinSeq(",", MakeAuditLogFragment(tx).Paths);
}

// Everything below reproduces what the hand-written switch produced, byte for
// byte. A change here is a change to what audit consumers read.
Y_UNIT_TEST(UnchangedFamiliesKeepTheirPaths) {
    {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpMkDir, "/MyRoot");
        tx.MutableMkDir()->SetName("dir");
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), "/MyRoot/dir");
    }
    {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpCreateTable, "/MyRoot");
        tx.MutableCreateTable()->SetName("T");
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), "/MyRoot/T");
    }
    {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpDropTable, "/MyRoot");
        tx.MutableDrop()->SetName("T");
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), "/MyRoot/T");
    }
    {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpAlterTable, "/MyRoot");
        tx.MutableAlterTable()->SetName("T");
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), "/MyRoot/T");
    }
    {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpModifyACL, "/MyRoot");
        tx.MutableModifyACL()->SetName("T");
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), "/MyRoot/T");
    }
    {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpCreateSubDomain, "/MyRoot");
        tx.MutableSubDomain()->SetName("db");
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), "/MyRoot/db");
    }
    {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpAlterUserAttributes, "/MyRoot");
        tx.MutableAlterUserAttributes()->SetPathName("sub");
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), "/MyRoot/sub");
    }
    {
        // Source and target, in that order, both absolute.
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpMoveTable, "/MyRoot");
        tx.MutableMoveTable()->SetSrcPath("/MyRoot/a");
        tx.MutableMoveTable()->SetDstPath("/MyRoot/b");
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), "/MyRoot/a,/MyRoot/b");
    }
    {
        // Two leaves under an absolute base.
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpMoveIndex, "/MyRoot");
        tx.MutableMoveIndex()->SetTablePath("/MyRoot/T");
        tx.MutableMoveIndex()->SetSrcPath("i1");
        tx.MutableMoveIndex()->SetDstPath("i2");
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), "/MyRoot/T/i1,/MyRoot/T/i2");
    }
    {
        // The table is the parent of what changes, so only the index shows up.
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpDropIndex, "/MyRoot");
        tx.MutableDropIndex()->SetTableName("T");
        tx.MutableDropIndex()->SetIndexName("i");
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), "/MyRoot/T/i");
    }
    {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpCreateCdcStream, "/MyRoot");
        tx.MutableCreateCdcStream()->SetTableName("T");
        tx.MutableCreateCdcStream()->MutableStreamDescription()->SetName("S");
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), "/MyRoot/T/S");
    }
    {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpDropCdcStream, "/MyRoot");
        tx.MutableDropCdcStream()->SetTableName("T");
        tx.MutableDropCdcStream()->AddStreamName("S1");
        tx.MutableDropCdcStream()->AddStreamName("S2");
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), "/MyRoot/T/S1,/MyRoot/T/S2");
    }
    {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpRotateCdcStream, "/MyRoot");
        tx.MutableRotateCdcStream()->SetTableName("T");
        tx.MutableRotateCdcStream()->SetOldStreamName("O");
        tx.MutableRotateCdcStream()->MutableNewStream()
            ->MutableStreamDescription()->SetName("N");
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), "/MyRoot/T/O,/MyRoot/T/N");
    }
    {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpCreateIndexBuild, "/MyRoot");
        tx.MutableInitiateIndexBuild()->SetTable("/MyRoot/T");
        tx.MutableInitiateIndexBuild()->MutableIndex()->SetName("i");
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), "/MyRoot/T/i");
    }
    {
        // The index and its impl table are dependencies of the create, not
        // paths the request names as changing.
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpCreateIndexedTable, "/MyRoot");
        tx.MutableCreateIndexedTable()->MutableTableDescription()->SetName("T");
        tx.MutableCreateIndexedTable()->AddIndexDescription()->SetName("i");
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), "/MyRoot/T");
    }
    {
        // No path field at all: the working dir is the audience being altered.
        // The login sub-message is set because the record's operation name is
        // derived from it, not because the paths depend on it.
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpAlterLogin, "/MyRoot");
        tx.MutableAlterLogin()->MutableCreateGroup()->SetGroup("g");
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), "/MyRoot");
    }
    {
        // Same shape for a different reason: the aggregator's working dir
        // already points at the backup collection.
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpCreateFullBackupOp,
            "/MyRoot/.backups/collections/c");
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), "/MyRoot/.backups/collections/c");
    }
    {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpIncrementalRestoreLockTargets, "/MyRoot");
        tx.MutableIncrementalRestoreLockTargets()->AddDstPaths("d");
        tx.MutableIncrementalRestoreLockTargets()->AddSrcPaths("s");
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), "/MyRoot/d,/MyRoot/s");
    }
}

Y_UNIT_TEST(IdAddressedRequestsKeepLegacyPaths) {
    {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpDropTable, "/MyRoot");
        tx.MutableDrop()->SetId(36);
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), "/MyRoot/");
        tx.MutableDrop()->SetName("T");
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), "/MyRoot/T");

        const auto refs = ExtractPathRefs(tx);
        UNIT_ASSERT(refs[0].Kind == EPathRefKind::ById);
        UNIT_ASSERT_VALUES_EQUAL(refs[0].LocalPathId, 36u);
        UNIT_ASSERT(JoinPathRef(tx.GetWorkingDir(), refs[0], {}).empty());
    }
    {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpAlterTable, "/MyRoot");
        TPathId(TOwnerId(72057594046678944ull), TLocalPathId(7)).ToProto(
            tx.MutableAlterTable()->MutablePathId());
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), "/MyRoot/");
        tx.MutableAlterTable()->SetName("T");
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), "/MyRoot/T");
    }
    {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpAlterTable, "/MyRoot");
        tx.MutableAlterTable()->SetId_Deprecated(7);
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), "/MyRoot/");
        tx.MutableAlterTable()->SetName("T");
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), "/MyRoot/T");
    }
    {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpAlterPersQueueGroup, "/MyRoot");
        tx.MutableAlterPersQueueGroup()->SetPathId(7);
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), "/MyRoot/");
        tx.MutableAlterPersQueueGroup()->SetName("topic");
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), "/MyRoot/topic");
    }
    {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpAlterBlockStoreVolume, "/MyRoot");
        tx.MutableAlterBlockStoreVolume()->SetPathId(7);
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), "/MyRoot/");
        tx.MutableAlterBlockStoreVolume()->SetName("volume");
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), "/MyRoot/volume");
    }
    {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpSplitMergeTablePartitions, "/MyRoot");
        tx.MutableSplitMergeTablePartitions()->SetTableOwnerId(72057594046678944ull);
        tx.MutableSplitMergeTablePartitions()->SetTableLocalId(7);
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), "/MyRoot/");
        tx.MutableSplitMergeTablePartitions()->SetTablePath("/MyRoot/T");
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), "/MyRoot//MyRoot/T");
    }
    for (const auto type : {NKikimrSchemeOp::ESchemeOpAlterReplication,
            NKikimrSchemeOp::ESchemeOpAlterTransfer}) {
        auto tx = MakeTx(type, "/MyRoot");
        TPathId(TOwnerId(72057594046678944ull), TLocalPathId(7)).ToProto(
            tx.MutableAlterReplication()->MutablePathId());
        UNIT_ASSERT(MakeAuditLogFragment(tx).Paths.empty());
        tx.MutableAlterReplication()->SetName("repl");
        UNIT_ASSERT(MakeAuditLogFragment(tx).Paths.empty());
    }
    for (const auto type : {NKikimrSchemeOp::ESchemeOpDropResourcePool,
            NKikimrSchemeOp::ESchemeOpDropStreamingQuery}) {
        auto tx = MakeTx(type, "/MyRoot");
        tx.MutableDrop()->SetId(7);
        const auto paths = MakeAuditLogFragment(tx).Paths;
        UNIT_ASSERT_VALUES_EQUAL(paths.size(), 1u);
        UNIT_ASSERT(paths[0].empty());
        tx.MutableDrop()->SetName("name");
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), "name");
    }
    {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpMkDir, "/MyRoot");
        tx.MutableMkDir()->SetName("dir");
        tx.AddApplyIf()->SetPathId(7);
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), "/MyRoot/dir");
    }
}

// These families logged a leaf name with no directory in front of it, because
// their switch arm forgot the working dir.
Y_UNIT_TEST(LeafNamesAreJoinedToTheWorkingDir) {
    const TString pools = "/MyRoot/.metadata/workload_manager/pools";
    {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpCreateResourcePool, pools);
        tx.MutableCreateResourcePool()->SetName("MyResourcePool");
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), pools + "/MyResourcePool");
    }
    {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpAlterResourcePool, pools);
        tx.MutableCreateResourcePool()->SetName("MyResourcePool");
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), pools + "/MyResourcePool");
    }
    {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpDropResourcePool, pools);
        tx.MutableDrop()->SetName("MyResourcePool");
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), pools + "/MyResourcePool");
    }
    {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpCreateStreamingQuery, "/MyRoot");
        tx.MutableCreateStreamingQuery()->SetName("Q");
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), "/MyRoot/Q");
    }
    {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpAlterStreamingQuery, "/MyRoot");
        tx.MutableCreateStreamingQuery()->SetName("Q");
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), "/MyRoot/Q");
    }
    {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpDropStreamingQuery, "/MyRoot");
        tx.MutableDrop()->SetName("Q");
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), "/MyRoot/Q");
    }
    {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpTruncateTable, "/MyRoot");
        tx.MutableTruncateTable()->SetTableName("T");
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), "/MyRoot/T");
    }
}

// SplitMerge resolves TablePath absolutely (split_merge.cpp:849); joining the
// working dir in front of it produced "/MyRoot//MyRoot/T".
Y_UNIT_TEST(AnAbsolutePathIsNotJoinedToTheWorkingDir) {
    auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpSplitMergeTablePartitions, "/MyRoot");
    tx.MutableSplitMergeTablePartitions()->SetTablePath("/MyRoot/T");
    UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), "/MyRoot/T");
}

// Four arms used to fall through with an empty body, so the record carried no
// "paths" field for an operation that changes exactly one path.
Y_UNIT_TEST(FamiliesThatUsedToReportNothing) {
    {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpAlterSequence, "/MyRoot");
        tx.MutableSequence()->SetName("seq");
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), "/MyRoot/seq");
    }
    {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpAlterReplication, "/MyRoot");
        tx.MutableAlterReplication()->SetName("repl");
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), "/MyRoot/repl");
    }
    {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpAlterTransfer, "/MyRoot");
        tx.MutableAlterReplication()->SetName("transfer");
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), "/MyRoot/transfer");
    }
    {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpAlterExternalTable, "/MyRoot");
        tx.MutableCreateExternalTable()->SetName("et");
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), "/MyRoot/et");
    }
    {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpAlterExternalDataSource, "/MyRoot");
        tx.MutableCreateExternalDataSource()->SetName("ds");
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), "/MyRoot/ds");
    }
}

// The create arm read AlterColumnTable.Name, a field a create request does not
// fill, so the record showed the bare working dir.
Y_UNIT_TEST(CreateColumnTableNamesWhatItCreates) {
    auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpCreateColumnTable, "/MyRoot");
    tx.MutableCreateColumnTable()->SetName("ct");
    UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), "/MyRoot/ct");
}

// New entries rather than corrected ones: a copy reads its source, and the
// extractor records that as a Source ref. The old switch dropped it.
Y_UNIT_TEST(CopySourcesAreReported) {
    {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpCreateTable, "/MyRoot");
        tx.MutableCreateTable()->SetName("dst");
        tx.MutableCreateTable()->SetCopyFromTable("/MyRoot/src");
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), "/MyRoot/dst,/MyRoot/src");
    }
    {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpCreateConsistentCopyTables, "/MyRoot");
        auto* item = tx.MutableCreateConsistentCopyTables()->AddCopyTableDescriptions();
        item->SetSrcPath("/MyRoot/src");
        item->SetDstPath("/MyRoot/dst");
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), "/MyRoot/src,/MyRoot/dst");
    }
}

// Also new entries: the continuous-backup family creates a cdc stream and an
// incremental backup table beside the table it names, and the request spells
// both out when the client chose their names.
Y_UNIT_TEST(ContinuousBackupReportsWhatItCreates) {
    {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpCreateContinuousBackup, "/MyRoot");
        tx.MutableCreateContinuousBackup()->SetTableName("T");
        tx.MutableCreateContinuousBackup()->MutableContinuousBackupDescription()
            ->SetStreamName("S");
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), "/MyRoot/T,/MyRoot/T/S");
    }
    {
        // Without a stream name the schemeshard generates one from the clock,
        // so there is nothing to report and the output is what it always was.
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpCreateContinuousBackup, "/MyRoot");
        tx.MutableCreateContinuousBackup()->SetTableName("T");
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), "/MyRoot/T");
    }
    {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpAlterContinuousBackup, "/MyRoot");
        tx.MutableAlterContinuousBackup()->SetTableName("T");
        auto& take = *tx.MutableAlterContinuousBackup()->MutableTakeIncrementalBackup();
        take.SetDstPath("bak");
        take.SetDstStreamPath("S");
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), "/MyRoot/T,/MyRoot/bak,/MyRoot/T/S");
    }
    {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpDropContinuousBackup, "/MyRoot");
        tx.MutableDropContinuousBackup()->SetTableName("T");
        UNIT_ASSERT_VALUES_EQUAL(AuditPaths(tx), "/MyRoot/T");
    }
}
}
