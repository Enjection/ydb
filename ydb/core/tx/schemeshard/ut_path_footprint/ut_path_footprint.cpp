#include <ydb/core/tx/schemeshard/schemeshard_path_footprint.h>
#include <ydb/core/tx/schemeshard/ut_helpers/helpers.h>

#include <google/protobuf/descriptor.h>

#include <library/cpp/logger/backend.h>
#include <library/cpp/logger/record.h>

#include <util/generic/algorithm.h>
#include <util/generic/hash.h>
#include <util/generic/hash_set.h>
#include <util/string/split.h>

using namespace NKikimr;
using namespace NKikimr::NSchemeShard;
using namespace NSchemeShardUT_Private;

namespace {

////////////////////////////////////////////////////////////////////////////////
// Layer-1 helpers

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

////////////////////////////////////////////////////////////////////////////////
// Observation channel: the "PathFootprint" LOG_NOTICE lines emitted by
// TSchemeShard::ProcessOperationParts. This is the least invasive channel:
// the hook itself is the only production edit, and tests need no extra seam.

// Collects each log record as its own string: TStreamLogBackend concatenates
// records without a separator, which makes line-based parsing impossible.
class TLogRecordCollector: public TLogBackend {
public:
    explicit TLogRecordCollector(TVector<TString>* sink)
        : Sink(sink)
    {}

    void WriteData(const TLogRecord& rec) override {
        Sink->emplace_back(rec.Data, rec.Len);
    }

    void ReopenLog() override {}

private:
    TVector<TString>* Sink;
};

struct TFootprintLine {
    THashMap<TString, TString> Fields;

    TString Get(TStringBuf key) const {
        const auto* v = Fields.FindPtr(TString(key));
        return v ? *v : TString();
    }
};

// The hook logs two layers with the same field grammar: "PathFootprint" for
// the derived parts and "PathFootprint request" for the client transactions
// they descend from. Every parse asks for exactly one of them.
TVector<TFootprintLine> ParseFootprintLogLayer(const TVector<TString>& records, size_t from,
        bool requestLayer)
{
    TVector<TFootprintLine> result;
    for (size_t i = from; i < records.size(); ++i) {
        const TStringBuf line = records[i];
        if (line.find("PathFootprint") == TStringBuf::npos) {
            continue;
        }
        if ((line.find("PathFootprint request") != TStringBuf::npos) != requestLayer) {
            continue;
        }
        TFootprintLine parsed;
        for (const auto& tokIt : StringSplitter(line).SplitByString(", ")) {
            TStringBuf tok = tokIt.Token();
            const size_t hash = tok.find('#');
            if (hash == TStringBuf::npos) {
                continue;
            }
            TStringBuf keyPart = tok.substr(0, hash);
            const size_t sp = keyPart.rfind(' ');
            const TStringBuf key = (sp == TStringBuf::npos) ? keyPart : keyPart.substr(sp + 1);
            TStringBuf value = tok.substr(hash + 1);
            while (value.starts_with(' ')) {
                value.remove_prefix(1);
            }
            parsed.Fields[TString(key)] = TString(value);
        }
        if (parsed.Fields.contains("fieldPath")) {
            result.push_back(std::move(parsed));
        }
    }
    return result;
}

TVector<TFootprintLine> ParseFootprintLog(const TVector<TString>& records, size_t from = 0) {
    return ParseFootprintLogLayer(records, from, /* requestLayer = */ false);
}

TVector<TFootprintLine> ParseRequestFootprintLog(const TVector<TString>& records, size_t from = 0) {
    return ParseFootprintLogLayer(records, from, /* requestLayer = */ true);
}

const TFootprintLine* FindLine(const TVector<TFootprintLine>& lines,
        TStringBuf opType, TStringBuf fieldPath)
{
    for (const auto& line : lines) {
        if (line.Get("partOpType") == opType && line.Get("fieldPath") == fieldPath) {
            return &line;
        }
    }
    return nullptr;
}

const TFootprintLine& RequireLine(const TVector<TFootprintLine>& lines,
        TStringBuf opType, TStringBuf fieldPath)
{
    const auto* found = FindLine(lines, opType, fieldPath);
    if (!found) {
        TStringBuilder dump;
        for (const auto& line : lines) {
            dump << "\n  " << line.Get("partOpType") << " / " << line.Get("fieldPath")
                 << " -> " << line.Get("absPath");
        }
        UNIT_FAIL("no PathFootprint line for " << opType << " / " << fieldPath << ", have:" << dump);
    }
    return *found;
}

TVector<TString> AbsPaths(const TVector<TFootprintLine>& lines,
        TStringBuf opType, TStringBuf fieldPath)
{
    TVector<TString> result;
    for (const auto& line : lines) {
        if (line.Get("partOpType") == opType && line.Get("fieldPath") == fieldPath) {
            result.push_back(line.Get("absPath"));
        }
    }
    Sort(result);
    return result;
}

////////////////////////////////////////////////////////////////////////////////
// Write set / publication helpers. Every part emits exactly one extra line
// with fieldPath "<writeSet>" carrying "owner:local" ids.

TVector<TString> SplitPathIds(const TString& joined) {
    TVector<TString> result;
    StringSplitter(joined).Split(',').SkipEmpty().Collect(&result);
    Sort(result);
    return result;
}

// "owner:local" of an existing path, in the same form the log uses. Private
// paths (index impl tables, cdc stream pq groups) need the private describe.
TString PathIdOf(TTestActorRuntime& runtime, const TString& path) {
    const auto& self = DescribePrivatePath(runtime, path).GetPathDescription().GetSelf();
    return TStringBuilder() << self.GetSchemeshardId() << ":" << self.GetPathId();
}

const TFootprintLine& RequireWriteSetLine(const TVector<TFootprintLine>& lines, TStringBuf opType) {
    for (const auto& line : lines) {
        if (line.Get("partOpType") == opType && line.Get("fieldPath") == "<writeSet>") {
            return line;
        }
    }
    UNIT_FAIL("no PathFootprint write set line for " << opType);
    return lines.front();
}

// Union of every part's write set, which is what a whole request wrote.
TVector<TString> AllWriteSetPathIds(const TVector<TFootprintLine>& lines) {
    THashSet<TString> seen;
    TVector<TString> result;
    for (const auto& line : lines) {
        if (line.Get("fieldPath") != "<writeSet>") {
            continue;
        }
        for (const TString& id : SplitPathIds(line.Get("writeSetPaths"))) {
            if (seen.insert(id).second) {
                result.push_back(id);
            }
        }
    }
    Sort(result);
    return result;
}

const TFootprintLine& RequireLineByAbsPath(const TVector<TFootprintLine>& lines,
        TStringBuf opType, TStringBuf absPath)
{
    for (const auto& line : lines) {
        if (line.Get("partOpType") == opType && line.Get("absPath") == absPath) {
            return line;
        }
    }
    UNIT_FAIL("no PathFootprint line for " << opType << " at " << absPath);
    return lines.front();
}

}  // namespace

Y_UNIT_TEST_SUITE(TSchemeShardPathFootprintExtract) {

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
        UNIT_ASSERT_VALUES_EQUAL(alterRefs.size(), 1u);
        CheckRef(alterRefs[0], "AlterColumnTable.Name", "ColumnTable",
            EPathRefKind::LeafUnderWorkingDir, EPathRefRole::Target);

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
            // A template is the identity of a field path: two rows rendering
            // the same string would be indistinguishable in a log line.
            UNIT_ASSERT_C(templates.insert(tmpl).second,
                "two path fields share the field-path template " << tmpl);

            // Rendering substitutes every placeholder and leaves no brace.
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

        // KnownPathFieldNames() is exactly the non-empty proto column,
        // deduplicated and sorted: the descriptor walk uses it as a set, and a
        // duplicate would hide a second field behind the first.
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

Y_UNIT_TEST_SUITE(TSchemeShardPathFootprintPropose) {

    Y_UNIT_TEST(CreateTableWithIntermediateDirs) {
        TVector<TString> log;
        TTestBasicRuntime runtime;
        runtime.SetLogBackend(new TLogRecordCollector(&log));
        TTestEnv env(runtime);
        ui64 txId = 100;

        const size_t mark = log.size();
        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "a/b/Table"
            Columns { Name: "key" Type: "Uint64" }
            Columns { Name: "value" Type: "Uint64" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        const auto lines = ParseFootprintLog(log);

        // Two auto-generated MkDir parts, then the CreateTable part.
        // (/MyRoot/.sys is created by the test env itself.)
        TVector<TString> mkdirs;
        for (const TString& path : AbsPaths(lines, "ESchemeOpMkDir", "MkDir.Name")) {
            if (!path.StartsWith("/MyRoot/.sys")) {
                mkdirs.push_back(path);
            }
        }
        UNIT_ASSERT_VALUES_EQUAL(mkdirs, (TVector<TString>{"/MyRoot/a", "/MyRoot/a/b"}));

        const auto& table = RequireLine(lines, "ESchemeOpCreateTable", "CreateTable.Name");
        UNIT_ASSERT_VALUES_EQUAL(table.Get("absPath"), "/MyRoot/a/b/Table");
        UNIT_ASSERT_VALUES_EQUAL(table.Get("kind"), "LeafUnderWorkingDir");
        UNIT_ASSERT_VALUES_EQUAL(table.Get("role"), "Target");
        UNIT_ASSERT_VALUES_EQUAL(table.Get("exists"), "0");  // not created yet at Propose
        UNIT_ASSERT_VALUES_EQUAL(table.Get("relToDb"), "a/b/Table");
        UNIT_ASSERT_VALUES_EQUAL(table.Get("relToWorkingDir"), "Table");
        UNIT_ASSERT_VALUES_EQUAL(table.Get("relToParent"), "Table");
        UNIT_ASSERT_VALUES_EQUAL(table.Get("workingDirRelToDb"), "a/b");
        UNIT_ASSERT_VALUES_EQUAL(table.Get("proposeStatus"), "StatusAccepted");

        // All three parts descend from the single client transaction.
        const auto ownLines = ParseFootprintLog(log, mark);
        for (const auto& line : ownLines) {
            UNIT_ASSERT_VALUES_EQUAL_C(line.Get("originalTxIndex"), "0",
                line.Get("partOpType") << " / " << line.Get("fieldPath"));
        }

        // ... and there is exactly one request footprint, describing the
        // request as the client wrote it: one multi-segment leaf name, not the
        // three derived parts.
        const auto requestLines = ParseRequestFootprintLog(log, mark);
        UNIT_ASSERT_VALUES_EQUAL(requestLines.size(), 1u);
        const auto& request = requestLines[0];
        UNIT_ASSERT_VALUES_EQUAL(request.Get("partId"), "<request>");
        UNIT_ASSERT_VALUES_EQUAL(request.Get("originalTxIndex"), "0");
        UNIT_ASSERT_VALUES_EQUAL(request.Get("partOpType"), "ESchemeOpCreateTable");
        UNIT_ASSERT_VALUES_EQUAL(request.Get("fieldPath"), "CreateTable.Name");
        UNIT_ASSERT_VALUES_EQUAL(request.Get("kind"), "LeafUnderWorkingDir");
        UNIT_ASSERT_VALUES_EQUAL(request.Get("relToDb"), "a/b/Table");
        UNIT_ASSERT_VALUES_EQUAL(request.Get("workingDir"), "/MyRoot");

        // The MkDir parts go through TMemoryChanges, so their write set is
        // exact: each new directory plus the parent whose child list changed.
        const TVector<TString> written = AllWriteSetPathIds(ownLines);
        for (const TString& path : {TString("/MyRoot"), TString("/MyRoot/a"), TString("/MyRoot/a/b")}) {
            const TString pathId = PathIdOf(runtime, path);
            UNIT_ASSERT_C(Find(written, pathId) != written.end(),
                "write set has no " << path << " (" << pathId << ")");
        }

        // TCreateTable::Propose writes straight through context.GetDB()
        // instead of recording TMemoryChanges, so its own write set is empty
        // and the part is flagged as a lower bound. The new table id is
        // therefore *not* in the write set above.
        const auto& createWriteSet = RequireWriteSetLine(ownLines, "ESchemeOpCreateTable");
        UNIT_ASSERT_VALUES_EQUAL(createWriteSet.Get("writeSet"), "0");
        UNIT_ASSERT_VALUES_EQUAL(createWriteSet.Get("incomplete"), "1");
        UNIT_ASSERT_VALUES_EQUAL(
            Find(written, PathIdOf(runtime, "/MyRoot/a/b/Table")) == written.end(), true);

        // The MkDir parts ran before any direct db write, so they are exact.
        const auto& mkdirWriteSet = RequireWriteSetLine(ownLines, "ESchemeOpMkDir");
        UNIT_ASSERT_VALUES_EQUAL(mkdirWriteSet.Get("incomplete"), "0");
        UNIT_ASSERT_VALUES_EQUAL(SplitPathIds(mkdirWriteSet.Get("writeSetPaths")).size(), 2u);
        UNIT_ASSERT(mkdirWriteSet.Get("published") != "0");
    }

    Y_UNIT_TEST(CreateIndexedTable) {
        TVector<TString> log;
        TTestBasicRuntime runtime;
        runtime.SetLogBackend(new TLogRecordCollector(&log));
        TTestEnv env(runtime);
        ui64 txId = 100;

        TestCreateIndexedTable(runtime, ++txId, "/MyRoot", R"(
            TableDescription {
                Name: "Table"
                Columns { Name: "key" Type: "Uint64" }
                Columns { Name: "value" Type: "Utf8" }
                KeyColumnNames: ["key"]
            }
            IndexDescription {
                Name: "byValue"
                KeyColumnNames: ["value"]
            }
        )");
        env.TestWaitNotification(runtime, txId);

        const auto lines = ParseFootprintLog(log);

        // The client request itself is not a part; the parts are the derived
        // CreateTable / CreateTableIndex / CreateTable(implTable) protos, each
        // already carrying an absolute WorkingDir and a leaf Name.
        UNIT_ASSERT_VALUES_EQUAL(
            AbsPaths(lines, "ESchemeOpCreateTable", "CreateTable.Name"),
            (TVector<TString>{"/MyRoot/Table", "/MyRoot/Table/byValue/indexImplTable"}));

        const auto& index = RequireLine(lines, "ESchemeOpCreateTableIndex", "CreateTableIndex.Name");
        UNIT_ASSERT_VALUES_EQUAL(index.Get("absPath"), "/MyRoot/Table/byValue");
        UNIT_ASSERT_VALUES_EQUAL(index.Get("relToDb"), "Table/byValue");

        const auto& impl = RequireLineByAbsPath(lines,
            "ESchemeOpCreateTable", "/MyRoot/Table/byValue/indexImplTable");
        UNIT_ASSERT_VALUES_EQUAL(impl.Get("relToWorkingDir"), "indexImplTable");
        UNIT_ASSERT_VALUES_EQUAL(impl.Get("workingDirRelToDb"), "Table/byValue");
    }

    Y_UNIT_TEST(CreateCdcStream) {
        TVector<TString> log;
        TTestBasicRuntime runtime;
        runtime.SetLogBackend(new TLogRecordCollector(&log));
        TTestEnv env(runtime, TTestEnvOptions().EnableProtoSourceIdInfo(true));
        ui64 txId = 100;

        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "Table"
            Columns { Name: "key" Type: "Uint64" }
            Columns { Name: "value" Type: "Uint64" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        const size_t mark = log.size();
        TestCreateCdcStream(runtime, ++txId, "/MyRoot", R"(
            TableName: "Table"
            StreamDescription {
              Name: "Stream"
              Mode: ECdcStreamModeKeysOnly
              Format: ECdcStreamFormatProto
            }
        )");
        env.TestWaitNotification(runtime, txId);

        const auto lines = ParseFootprintLog(log, mark);

        const auto& atTable = RequireLine(lines,
            "ESchemeOpCreateCdcStreamAtTable", "CreateCdcStream.TableName");
        UNIT_ASSERT_VALUES_EQUAL(atTable.Get("absPath"), "/MyRoot/Table");
        UNIT_ASSERT_VALUES_EQUAL(atTable.Get("exists"), "1");
        UNIT_ASSERT(atTable.Get("pathId") != "");

        const auto& impl = RequireLine(lines,
            "ESchemeOpCreateCdcStreamImpl", "CreateCdcStream.StreamDescription.Name");
        UNIT_ASSERT_VALUES_EQUAL(impl.Get("absPath"), "/MyRoot/Table/Stream");
        UNIT_ASSERT_VALUES_EQUAL(impl.Get("relToDb"), "Table/Stream");

        // The AtTable part resolves the stream leaf too (it fills
        // txState.CdcPathId from it), so the footprint must report it.
        const auto& atTableStream = RequireLine(lines,
            "ESchemeOpCreateCdcStreamAtTable", "CreateCdcStream.StreamDescription.Name");
        UNIT_ASSERT_VALUES_EQUAL(atTableStream.Get("absPath"), "/MyRoot/Table/Stream");
        UNIT_ASSERT_VALUES_EQUAL(atTableStream.Get("kind"), "LeafUnderSibling");
        UNIT_ASSERT_VALUES_EQUAL(atTableStream.Get("relToWorkingDir"), "Table/Stream");
    }

    Y_UNIT_TEST(MoveTable) {
        TVector<TString> log;
        TTestBasicRuntime runtime;
        runtime.SetLogBackend(new TLogRecordCollector(&log));
        TTestEnv env(runtime);
        ui64 txId = 100;

        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "Table"
            Columns { Name: "key" Type: "Uint64" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        const size_t mark = log.size();
        TestMoveTable(runtime, ++txId, "/MyRoot/Table", "/MyRoot/Moved");
        env.TestWaitNotification(runtime, txId);

        const auto lines = ParseFootprintLog(log, mark);

        const auto& src = RequireLine(lines, "ESchemeOpMoveTable", "MoveTable.SrcPath");
        UNIT_ASSERT_VALUES_EQUAL(src.Get("absPath"), "/MyRoot/Table");
        UNIT_ASSERT_VALUES_EQUAL(src.Get("role"), "Source");
        UNIT_ASSERT_VALUES_EQUAL(src.Get("exists"), "1");

        const auto& dst = RequireLine(lines, "ESchemeOpMoveTable", "MoveTable.DstPath");
        UNIT_ASSERT_VALUES_EQUAL(dst.Get("absPath"), "/MyRoot/Moved");
        UNIT_ASSERT_VALUES_EQUAL(dst.Get("role"), "Target");
        UNIT_ASSERT_VALUES_EQUAL(dst.Get("exists"), "0");
    }

    Y_UNIT_TEST(DropTableByNameAndById) {
        TVector<TString> log;
        TTestBasicRuntime runtime;
        runtime.SetLogBackend(new TLogRecordCollector(&log));
        TTestEnv env(runtime);
        ui64 txId = 100;

        for (const TString& name : {TString("ByName"), TString("ById")}) {
            TestCreateTable(runtime, ++txId, "/MyRoot", Sprintf(R"(
                Name: "%s"
                Columns { Name: "key" Type: "Uint64" }
                KeyColumnNames: ["key"]
            )", name.c_str()));
            env.TestWaitNotification(runtime, txId);
        }

        const auto describe = DescribePath(runtime, "/MyRoot/ById");
        const ui64 localPathId = describe.GetPathDescription().GetSelf().GetPathId();

        size_t mark = log.size();
        TestDropTable(runtime, ++txId, "/MyRoot", "ByName");
        env.TestWaitNotification(runtime, txId);
        {
            const auto lines = ParseFootprintLog(log, mark);
            const auto& drop = RequireLine(lines, "ESchemeOpDropTable", "Drop.Name");
            UNIT_ASSERT_VALUES_EQUAL(drop.Get("absPath"), "/MyRoot/ByName");
            UNIT_ASSERT_VALUES_EQUAL(drop.Get("kind"), "LeafUnderWorkingDir");
            UNIT_ASSERT_VALUES_EQUAL(drop.Get("exists"), "1");
        }

        mark = log.size();
        TestDropTable(runtime, ++txId, localPathId);
        env.TestWaitNotification(runtime, txId);
        {
            const auto lines = ParseFootprintLog(log, mark);
            const auto& drop = RequireLine(lines, "ESchemeOpDropTable", "Drop.Id");
            UNIT_ASSERT_VALUES_EQUAL(drop.Get("kind"), "ById");
            UNIT_ASSERT_VALUES_EQUAL(drop.Get("absPath"), "/MyRoot/ById");
            UNIT_ASSERT_VALUES_EQUAL(drop.Get("exists"), "1");
        }
    }

    Y_UNIT_TEST(ConsistentCopyTables) {
        TVector<TString> log;
        TTestBasicRuntime runtime;
        runtime.SetLogBackend(new TLogRecordCollector(&log));
        TTestEnv env(runtime);
        ui64 txId = 100;

        for (int i = 0; i < 2; ++i) {
            TestCreateTable(runtime, ++txId, "/MyRoot", Sprintf(R"(
                Name: "Src%d"
                Columns { Name: "key" Type: "Uint64" }
                KeyColumnNames: ["key"]
            )", i));
            env.TestWaitNotification(runtime, txId);
        }

        const size_t mark = log.size();
        TestConsistentCopyTables(runtime, ++txId, "/MyRoot", R"(
            CopyTableDescriptions { SrcPath: "/MyRoot/Src0" DstPath: "/MyRoot/Dst0" }
            CopyTableDescriptions { SrcPath: "/MyRoot/Src1" DstPath: "/MyRoot/Dst1" }
        )");
        env.TestWaitNotification(runtime, txId);

        const auto lines = ParseFootprintLog(log, mark);

        // Each item becomes its own CreateTable part with an absolute
        // WorkingDir and a leaf Name; both destinations must be present.
        TVector<TString> created;
        for (const auto& line : lines) {
            if (line.Get("partOpType") == "ESchemeOpCreateTable" &&
                line.Get("fieldPath") == "CreateTable.Name")
            {
                created.push_back(line.Get("absPath"));
            }
        }
        Sort(created);
        UNIT_ASSERT_VALUES_EQUAL(created, (TVector<TString>{"/MyRoot/Dst0", "/MyRoot/Dst1"}));
    }

    // A backup collection's ExplicitEntryList entries are an Absolute field:
    // RegisterBackupCollectionTables() resolves each with TPath::Resolve() and
    // never joins WorkingDir (schemeshard_impl.cpp:3920). Layer 2 must do the
    // same even when the value has no leading slash, otherwise it invents a
    // path under the working dir that the operation never touches.
    Y_UNIT_TEST(BackupCollectionEntriesAreAbsoluteNotWorkingDirRelative) {
        TVector<TString> log;
        TTestBasicRuntime runtime;
        runtime.SetLogBackend(new TLogRecordCollector(&log));
        TTestEnv env(runtime, TTestEnvOptions().EnableBackupService(true));
        ui64 txId = 100;

        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "Table1"
            Columns { Name: "key" Type: "Uint64" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        // The collection root is not auto-created.
        TestMkDir(runtime, ++txId, "/MyRoot", ".backups");
        env.TestWaitNotification(runtime, txId);
        TestMkDir(runtime, ++txId, "/MyRoot/.backups", "collections");
        env.TestWaitNotification(runtime, txId);

        const size_t mark = log.size();
        TestCreateBackupCollection(runtime, ++txId, "/MyRoot/.backups/collections/", R"(
            Name: "MyCollection"
            ExplicitEntryList {
                Entries { Type: ETypeTable Path: "/MyRoot/Table1" }
                Entries { Type: ETypeTable Path: "Table1" }
            }
            Cluster: {}
        )");
        env.TestWaitNotification(runtime, txId);

        const auto lines = ParseFootprintLog(log, mark);

        const auto& absolute = RequireLine(lines, "ESchemeOpCreateBackupCollection",
            "CreateBackupCollection.ExplicitEntryList.Entries[0].Path");
        UNIT_ASSERT_VALUES_EQUAL(absolute.Get("kind"), "Absolute");
        UNIT_ASSERT_VALUES_EQUAL(absolute.Get("role"), "Dependency");
        UNIT_ASSERT_VALUES_EQUAL(absolute.Get("absPath"), "/MyRoot/Table1");
        UNIT_ASSERT_VALUES_EQUAL(absolute.Get("exists"), "1");

        // No leading slash, but still not joined with the working dir.
        const auto& relative = RequireLine(lines, "ESchemeOpCreateBackupCollection",
            "CreateBackupCollection.ExplicitEntryList.Entries[1].Path");
        UNIT_ASSERT_VALUES_EQUAL(relative.Get("absPath"), "/Table1");
        UNIT_ASSERT_VALUES_EQUAL(relative.Get("exists"), "0");
    }

    Y_UNIT_TEST(RejectedCreateTableStillProducesFootprint) {
        TVector<TString> log;
        TTestBasicRuntime runtime;
        runtime.SetLogBackend(new TLogRecordCollector(&log));
        TTestEnv env(runtime);
        ui64 txId = 100;

        const size_t mark = log.size();
        TestCreateTable(runtime, ++txId, "/MyRoot/NoSuchDir", R"(
            Name: "Table"
            Columns { Name: "key" Type: "Uint64" }
            KeyColumnNames: ["key"]
        )", {NKikimrScheme::StatusPathDoesNotExist});

        const auto lines = ParseFootprintLog(log, mark);
        const auto& table = RequireLine(lines, "ESchemeOpCreateTable", "CreateTable.Name");
        UNIT_ASSERT_VALUES_EQUAL(table.Get("absPath"), "/MyRoot/NoSuchDir/Table");
        UNIT_ASSERT_VALUES_EQUAL(table.Get("exists"), "0");
        UNIT_ASSERT_VALUES_EQUAL(table.Get("proposeStatus"), "StatusPathDoesNotExist");
        // Best effort even though nothing under the working dir resolves.
        UNIT_ASSERT_VALUES_EQUAL(table.Get("relToDb"), "NoSuchDir/Table");

        // A part that fails its checks never gets as far as writing anything.
        const auto& writeSet = RequireWriteSetLine(lines, "ESchemeOpCreateTable");
        UNIT_ASSERT_VALUES_EQUAL(writeSet.Get("writeSet"), "0");
        UNIT_ASSERT_VALUES_EQUAL(writeSet.Get("published"), "0");
        UNIT_ASSERT_VALUES_EQUAL(writeSet.Get("incomplete"), "0");
        UNIT_ASSERT_VALUES_EQUAL(AllWriteSetPathIds(lines), (TVector<TString>{}));
    }

    // Dropping an indexed table names only the table, but the operation
    // touches the index and its impl table too. Those cascaded paths appear in
    // the write set although no proto field of the request mentions them.
    Y_UNIT_TEST(DropIndexedTableWriteSetCoversTheCascade) {
        TVector<TString> log;
        TTestBasicRuntime runtime;
        runtime.SetLogBackend(new TLogRecordCollector(&log));
        TTestEnv env(runtime);
        ui64 txId = 100;

        TestCreateIndexedTable(runtime, ++txId, "/MyRoot", R"(
            TableDescription {
                Name: "Table"
                Columns { Name: "key" Type: "Uint64" }
                Columns { Name: "value" Type: "Utf8" }
                KeyColumnNames: ["key"]
            }
            IndexDescription {
                Name: "byValue"
                KeyColumnNames: ["value"]
            }
        )");
        env.TestWaitNotification(runtime, txId);

        const TString table = PathIdOf(runtime, "/MyRoot/Table");
        const TString index = PathIdOf(runtime, "/MyRoot/Table/byValue");
        const TString implTable = PathIdOf(runtime, "/MyRoot/Table/byValue/indexImplTable");

        const size_t mark = log.size();
        TestDropTable(runtime, ++txId, "/MyRoot", "Table");
        env.TestWaitNotification(runtime, txId);

        const auto lines = ParseFootprintLog(log, mark);
        const TVector<TString> written = AllWriteSetPathIds(lines);
        for (const auto& [name, pathId] : TVector<std::pair<TString, TString>>{
                {"/MyRoot", PathIdOf(runtime, "/MyRoot")},
                {"/MyRoot/Table", table},
                {"/MyRoot/Table/byValue", index},
                {"/MyRoot/Table/byValue/indexImplTable", implTable}})
        {
            UNIT_ASSERT_C(Find(written, pathId) != written.end(),
                "write set has no " << name << " (" << pathId << ")");
        }
    }

    // Two transactions in one request: every part carries the index of the
    // client transaction it descends from, and each gets its own request
    // footprint.
    Y_UNIT_TEST(TwoTransactionsGetDistinctOriginalTxIndexes) {
        TVector<TString> log;
        TTestBasicRuntime runtime;
        runtime.SetLogBackend(new TLogRecordCollector(&log));
        TTestEnv env(runtime);
        ui64 txId = 100;

        const size_t mark = log.size();
        ++txId;
        {
            auto* request = new TEvTx(txId, TTestTxConfig::SchemeShard);
            for (const TString& name : {TString("first"), TString("second/nested")}) {
                auto& tx = *request->Record.AddTransaction();
                tx.SetOperationType(NKikimrSchemeOp::ESchemeOpMkDir);
                tx.SetWorkingDir("/MyRoot");
                tx.MutableMkDir()->SetName(name);
            }
            AsyncSend(runtime, TTestTxConfig::SchemeShard, request);
            TestModificationResults(runtime, txId, {{NKikimrScheme::StatusAccepted}});
        }
        env.TestWaitNotification(runtime, txId);

        const auto requestLines = ParseRequestFootprintLog(log, mark);
        UNIT_ASSERT_VALUES_EQUAL(requestLines.size(), 2u);
        UNIT_ASSERT_VALUES_EQUAL(requestLines[0].Get("originalTxIndex"), "0");
        UNIT_ASSERT_VALUES_EQUAL(requestLines[0].Get("absPath"), "/MyRoot/first");
        UNIT_ASSERT_VALUES_EQUAL(requestLines[1].Get("originalTxIndex"), "1");
        UNIT_ASSERT_VALUES_EQUAL(requestLines[1].Get("absPath"), "/MyRoot/second/nested");

        // The second transaction fans out into a generated MkDir for "second"
        // plus the MkDir for "nested"; all of them point back at index 1.
        const auto lines = ParseFootprintLog(log, mark);
        THashMap<TString, TString> originByAbsPath;
        for (const auto& line : lines) {
            if (line.Get("fieldPath") == "MkDir.Name") {
                originByAbsPath[line.Get("absPath")] = line.Get("originalTxIndex");
            }
        }
        UNIT_ASSERT_VALUES_EQUAL(originByAbsPath["/MyRoot/first"], "0");
        UNIT_ASSERT_VALUES_EQUAL(originByAbsPath["/MyRoot/second"], "1");
        UNIT_ASSERT_VALUES_EQUAL(originByAbsPath["/MyRoot/second/nested"], "1");
    }
}

////////////////////////////////////////////////////////////////////////////////
// Descriptor-walk completeness (research plan §8.4). Turns "the extractor knows
// every path field" from a snapshot into an invariant: a new path-like string
// field anywhere under TModifyScheme fails this test until it is either read by
// the extractor or explicitly classified here.

namespace {

// Substrings that make a string field look like it could carry a path.
// Case-sensitive, matched against the protobuf field name.
const TVector<TStringBuf> PathLikeSubstrings = {
    "Path", "Name", "Dir", "Table", "From", "Src", "Dst", "Prefix",
};

bool LooksLikeAPathField(const google::protobuf::FieldDescriptor* field) {
    if (field->type() != google::protobuf::FieldDescriptor::TYPE_STRING) {
        return false;  // bytes, ints and messages are out of scope
    }
    const TStringBuf name(field->name());
    for (const auto& needle : PathLikeSubstrings) {
        if (name.find(needle) != TStringBuf::npos) {
            return true;
        }
    }
    return false;
}

// Packages that are walked into but never classified: they describe transport
// types, actor ids and public-API values, none of which SchemeShard resolves as
// a path in its own tree. Anything a SchemeShard operation resolves lives in
// NKikimrSchemeOp or in one of the component packages it embeds (NKikimrPQ,
// NKikimrSubDomains, NKikimrReplication, NKikimrIndexBuilder, ...), all of
// which are classified below.
bool IsSkippedPackage(TStringBuf package) {
    static const THashSet<TString> skipped = {
        "Ydb",
        "Ydb.Table",
        "Ydb.Topic",
        "NKikimrProto",
        "NActorsProto",
        "google.protobuf",
        "NYql",
        "NYql.NProto",
    };
    if (skipped.contains(TString(package))) {
        return true;
    }
    // Any Ydb.* public API subpackage.
    return package.StartsWith("Ydb.");
}

void CollectPathLikeFields(const google::protobuf::Descriptor* descriptor,
        THashSet<TString>& visited, TVector<TString>& out)
{
    if (!descriptor || !visited.insert(TString(descriptor->full_name())).second) {
        return;
    }
    const bool classify = !IsSkippedPackage(descriptor->file()->package());
    for (int i = 0; i < descriptor->field_count(); ++i) {
        const auto* field = descriptor->field(i);
        if (classify && LooksLikeAPathField(field)) {
            out.push_back(TString(field->full_name()));
        }
        if (field->type() == google::protobuf::FieldDescriptor::TYPE_MESSAGE
                || field->type() == google::protobuf::FieldDescriptor::TYPE_GROUP) {
            // Map fields are ordinary repeated messages here: recursing into
            // the synthesized entry type reaches the map's value message.
            CollectPathLikeFields(field->message_type(), visited, out);
        }
    }
}

TString Dump(const TVector<TString>& names) {
    TStringBuilder out;
    for (const auto& name : names) {
        out << "\n        \"" << name << "\",";
    }
    return out;
}

}  // namespace

Y_UNIT_TEST_SUITE(TSchemeShardPathFootprintProtoCoverage) {

    // The base path of every request. It is reported as TPathFootprint::
    // WorkingDir and WorkingDirRelToDb, not as an entry in the ref list.
    const THashSet<TString> ReportedOutsideTheRefList = {
        "NKikimrSchemeOp.TModifyScheme.WorkingDir",
    };

    // Path-like string fields under TModifyScheme that are not paths in this
    // scheme tree. Each was classified by reading the field's use; the grouping
    // comment says why.
    const THashSet<TString> NotAPath = {
        // Column, family and key names inside a table or column-table schema.
        "NKikimrIndexBuilder.TColumnBuildSetting.ColumnName",
        "NKikimrPQ.TPQTabletConfig.TKeyComponentSchema.Name",
        "NKikimrSchemeOp.TColumnDataLifeCycle.TTtl.ColumnName",
        "NKikimrSchemeOp.TColumnDescription.FamilyName",
        "NKikimrSchemeOp.TColumnDescription.Name",
        "NKikimrSchemeOp.TColumnTableSchema.KeyColumnNames",
        "NKikimrSchemeOp.TDefaultExpressionColumnDescription.DependencyColumnNames",
        "NKikimrSchemeOp.TFamilyDescription.Name",
        "NKikimrSchemeOp.TIndexAlteringConfig.DataColumnNames",
        "NKikimrSchemeOp.TIndexAlteringConfig.KeyColumnNames",
        "NKikimrSchemeOp.TIndexCreationConfig.DataColumnNames",
        "NKikimrSchemeOp.TIndexCreationConfig.KeyColumnNames",
        "NKikimrSchemeOp.TIndexDataExtractor.TSubColumn.SubColumnName",
        "NKikimrSchemeOp.TIndexDescription.DataColumnNames",
        "NKikimrSchemeOp.TIndexDescription.KeyColumnNames",
        "NKikimrSchemeOp.TMultiColumnStatisticsDescription.ColumnNames",
        "NKikimrSchemeOp.TOlapColumnDescription.ColumnFamilyName",
        "NKikimrSchemeOp.TOlapColumnDescription.Name",
        "NKikimrSchemeOp.TOlapColumnDiff.ColumnFamilyName",
        "NKikimrSchemeOp.TOlapColumnDiff.Name",
        "NKikimrSchemeOp.TRequestedBloomFilter.ColumnNames",
        "NKikimrSchemeOp.TRequestedBloomNGrammFilter.ColumnName",
        "NKikimrSchemeOp.TRequestedCountMinSketch.ColumnNames",
        "NKikimrSchemeOp.TRequestedMaxIndex.ColumnName",
        "NKikimrSchemeOp.TRequestedMinMaxIndex.ColumnName",
        "NKikimrSchemeOp.TTTLSettings.TEnabled.ColumnName",
        "NKikimrSchemeOp.TTableDescription.KeyColumnNames",
        // Objects named inside a column table (presets, olap indexes, statistics).
        // They live in the table's schema, not as children in the scheme tree.
        "NKikimrSchemeOp.TAlterColumnTable.AlterSchemaPresetName",
        "NKikimrSchemeOp.TAlterColumnTable.RESERVED_AlterTtlSettingsPresetName",
        "NKikimrSchemeOp.TAlterColumnTableSchemaPreset.Name",
        "NKikimrSchemeOp.TAlterColumnTableTtlSettingsPreset.Name",
        "NKikimrSchemeOp.TColumnTableDescription.SchemaPresetName",
        "NKikimrSchemeOp.TColumnTableSchemaPreset.Name",
        "NKikimrSchemeOp.TColumnTableTtlSettingsPreset.Name",
        "NKikimrSchemeOp.TIndexDescription.Name",
        "NKikimrSchemeOp.TMultiColumnStatisticsDescription.Name",
        "NKikimrSchemeOp.TOlapIndexDescription.Name",
        "NKikimrSchemeOp.TOlapIndexRequested.Name",
        "NKikimrSchemeOp.TOlapMoveIndex.DestinationName",
        "NKikimrSchemeOp.TOlapMoveIndex.SourceName",
        "NKikimrSchemeOp.TRemoveColumnTableSchemaPreset.Name",
        "NKikimrSchemeOp.TRemoveColumnTableTtlSettingsPreset.Name",
        // Registered C++ class, policy and logic names looked up in a factory.
        "NKikimrArrowAccessorProto.TConstructor.ClassName",
        "NKikimrArrowAccessorProto.TDataExtractor.ClassName",
        "NKikimrArrowAccessorProto.TRequestedConstructor.ClassName",
        "NKikimrSchemeOp.TColumnTableRequestedOptions.ScanReaderPolicyName",
        "NKikimrSchemeOp.TColumnTableSchemeOptions.ScanReaderPolicyName",
        "NKikimrSchemeOp.TCompactionLevelConstructorContainer.ClassName",
        "NKikimrSchemeOp.TCompactionLevelConstructorContainer.DefaultSelectorName",
        "NKikimrSchemeOp.TCompactionPlannerConstructorContainer.ClassName",
        "NKikimrSchemeOp.TCompactionPlannerConstructorContainer.TSOptimizer.LogicName",
        "NKikimrSchemeOp.TCompactionSelectorConstructorContainer.ClassName",
        "NKikimrSchemeOp.TCompactionSelectorConstructorContainer.Name",
        "NKikimrSchemeOp.TIndexDataExtractor.ClassName",
        "NKikimrSchemeOp.TMetadataManagerConstructorContainer.ClassName",
        "NKikimrSchemeOp.TOlapColumn.TSerializer.ClassName",
        "NKikimrSchemeOp.TOlapIndexDescription.ClassName",
        "NKikimrSchemeOp.TOlapIndexRequested.ClassName",
        "NKikimrSchemeOp.TPartitionConfig.NamedCompactionPolicy",
        "NKikimrSchemeOp.TSkipIndexBitSetStorage.ClassName",
        // Storage pools and channel profiles: BS group selectors, not scheme paths.
        "NKikimrBlobDepot.TBlobDepotConfig.Name",
        "NKikimrBlobDepot.TChannelProfile.StoragePoolName",
        "NKikimrBlockStore.TVolumeConfig.StoragePoolName",
        "NKikimrStoragePool.TChannelBind.StoragePoolName",
        "NKikimrStoragePool.TStoragePool.Name",
        // Credentials and secret references. A secret is resolved by the secrets
        // subsystem by name, not as a TPath by any of these operations.
        "NKikimrReplication.TOAuthToken.TokenSecretName",
        "NKikimrReplication.TStaticCredentials.PasswordSecretName",
        "NKikimrSchemeOp.TAws.AwsAccessKeyIdSecretName",
        "NKikimrSchemeOp.TAws.AwsSecretAccessKeySecretName",
        "NKikimrSchemeOp.TBasic.PasswordSecretName",
        "NKikimrSchemeOp.TIamImpersonate.InitialTokenSecretName",
        "NKikimrSchemeOp.TMdbBasic.PasswordSecretName",
        "NKikimrSchemeOp.TMdbBasic.ServiceAccountSecretName",
        "NKikimrSchemeOp.TSecretSchemaOp.ValueParamName",
        "NKikimrSchemeOp.TServiceAccountAuth.SecretName",
        "NKikimrSchemeOp.TToken.TokenSecretName",
        // Locations outside this scheme tree: the remote replication cluster, a
        // filesystem or YT export target, an SQS queue.
        "NKikimrPQ.TPQTabletConfig.SqsAccountName",
        "NKikimrPQ.TPQTabletConfig.SqsQueueName",
        "NKikimrPQ.TPQTabletConfig.TConsumer.Name",
        "NKikimrReplication.TReplicationConfig.TTargetSpecific.TTarget.SrcPath",
        "NKikimrReplication.TReplicationConfig.TTargetSpecific.TTarget.SrcStreamName",
        "NKikimrReplication.TReplicationConfig.TTransferSpecific.TTarget.ConsumerName",
        "NKikimrReplication.TReplicationConfig.TTransferSpecific.TTarget.DstPathLambda",
        "NKikimrReplication.TReplicationConfig.TTransferSpecific.TTarget.SrcPath",
        "NKikimrSchemeOp.TFSSettings.BasePath",
        "NKikimrSchemeOp.TFSSettings.Path",
        "NKikimrSchemeOp.TYTSettings.TablePattern",
        // Filled in by SchemeShard from an already resolved path, or returned by
        // Describe. Never read from the request, so never a path to extract.
        "NKikimrPQ.TPQTabletConfig.TopicName",
        "NKikimrPQ.TPQTabletConfig.TopicPath",
        "NKikimrPQ.TPQTabletConfig.YdbDatabasePath",
        "NKikimrReplication.TReplicationLocationConfig.Path",
        "NKikimrSchemeOp.TDirEntry.Name",
        "NKikimrSchemeOp.TExternalTableReferences.TReference.Path",
        "NKikimrSchemeOp.TSecretDescription.Name",
        "NKikimrSchemeOp.TSolomonVolumeDescription.Name",
        "NKikimrSchemeOp.TTableDescription.Path",
        "NKikimrSchemeOp.TTestShardSetDescription.Name",
        // Login objects and a character set: neither is addressed by path.
        "NKikimrSchemeOp.TLoginRenameGroup.NewName",
        "NKikimrSubDomains.TSchemeLimits.ExtraPathSymbolsAllowed",
        "NLoginProto.TSid.Name",
    };

    // Real paths, or components of one, that no Propose() the extractor covers
    // resolves, so it deliberately reports nothing for them. Tolerated, but
    // printed on every run so the list stays under review.
    const THashSet<TString> Unclassified = {
        // Path components joined into the derived parts' own path fields, which the
        // footprint does extract; the component itself is never resolved alone.
        "NKikimrSchemeOp.TBackupBackupCollection.TargetDir",
        "NKikimrSchemeOp.TBackupCollectionDescription.Prefix",
        // Absolute paths that only later execution states resolve. The hook runs at
        // Propose, so they are outside the footprint by construction (plan §8.2.4).
        "NKikimrReplication.TReplicationConfig.TTargetEverything.DstPrefix",
        "NKikimrSchemeOp.TIncrementalRestoreFinalize.BackupTablePaths",
        "NKikimrSchemeOp.TIncrementalRestoreFinalize.TargetTablePaths",
        // TModifyScheme submessages that no EOperationType dispatches to, so no
        // Propose reads them at all.
        "NKikimrSchemeOp.TPersQueueGroupAllocate.Name",
        "NKikimrSchemeOp.TPersQueueGroupDeallocate.Name",
    };

    Y_UNIT_TEST(EveryPathLikeFieldIsClassified) {
        THashSet<TString> visited;
        TVector<TString> collected;
        CollectPathLikeFields(NKikimrSchemeOp::TModifyScheme::descriptor(), visited, collected);
        Sort(collected);
        UNIT_ASSERT_C(collected.size() > 100,
            "the descriptor walk found only " << collected.size() << " path-like fields");

        const THashSet<TString> known(KnownPathFieldNames().begin(), KnownPathFieldNames().end());
        const THashSet<TString> reachable(collected.begin(), collected.end());

        // A typo or a renamed proto field would silently shrink the known set.
        TVector<TString> stale;
        for (const auto& name : known) {
            if (!reachable.contains(name)) {
                stale.push_back(name);
            }
        }
        Sort(stale);
        UNIT_ASSERT_C(stale.empty(),
            "KnownPathFieldNames() lists fields the descriptor walk cannot reach"
            " (renamed, misspelled, or no longer path-like):" << Dump(stale));

        TVector<TString> unclassified;
        TVector<TString> uncovered;
        for (const auto& name : collected) {
            if (known.contains(name) || NotAPath.contains(name)
                    || ReportedOutsideTheRefList.contains(name)) {
                continue;
            }
            if (Unclassified.contains(name)) {
                unclassified.push_back(name);
                continue;
            }
            uncovered.push_back(name);
        }

        if (!unclassified.empty()) {
            Cerr << "PathFootprint: " << unclassified.size()
                 << " tolerated unclassified path-like fields:" << Dump(unclassified) << Endl;
        }

        UNIT_ASSERT_C(uncovered.empty(), "" << uncovered.size()
            << " path-like field(s) of TModifyScheme are neither read by"
            " ExtractPathRefs nor classified in this test. Read each field's use,"
            " then add it to the extractor, to NotAPath, or to Unclassified:"
            << Dump(uncovered));
    }
}
