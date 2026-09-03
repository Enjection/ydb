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
// Observation channel: TAppData::PathFootprintObserver, installed before the
// schemeshard boots by TTestEnvOptions::PathFootprintObserver. The observer
// gets the TPathFootprint itself, so assertions are typed; the DEBUG log line
// is a rendering of the same struct and is pinned by its own test below.

struct TObservedFootprint {
    TTxId TxId;
    TPathFootprint Footprint;
};

class TFootprintCollector: public IPathFootprintObserver {
public:
    void OnRequestFootprint(TTxId txId, const TPathFootprint& footprint) override {
        Requests.push_back(TObservedFootprint{txId, footprint});
    }

    void OnPartFootprint(TTxId txId, const TPathFootprint& footprint) override {
        Parts.push_back(TObservedFootprint{txId, footprint});
    }

    // A TDeque never relocates, so a TObservedEntry taken before more parts
    // arrive stays valid.
    TDeque<TObservedFootprint> Requests;
    TDeque<TObservedFootprint> Parts;
};

// One (footprint, entry) pair, flattened out of the observed footprints the
// way the log used to flatten them into lines. Entry is null for a footprint
// whose request names no path at all.
struct TObservedEntry {
    const TPathFootprint* Part = nullptr;
    const TPathFootprintEntry* Entry = nullptr;

    TString OpType() const {
        return NKikimrSchemeOp::EOperationType_Name(Part->PartOpType);
    }

    TString FieldPath() const {
        return Entry ? Entry->Ref.FieldPath : TString();
    }

    TString AbsPath() const {
        return Entry ? Entry->AbsPath : TString();
    }
};

TVector<TObservedEntry> Flatten(const TDeque<TObservedFootprint>& footprints, size_t from = 0) {
    TVector<TObservedEntry> result;
    for (size_t i = from; i < footprints.size(); ++i) {
        const TPathFootprint& footprint = footprints[i].Footprint;
        if (footprint.Entries.empty()) {
            result.push_back(TObservedEntry{&footprint, nullptr});
            continue;
        }
        for (const auto& entry : footprint.Entries) {
            result.push_back(TObservedEntry{&footprint, &entry});
        }
    }
    return result;
}

const TObservedEntry* FindEntry(const TVector<TObservedEntry>& entries,
        TStringBuf opType, TStringBuf fieldPath)
{
    for (const auto& observed : entries) {
        if (observed.OpType() == opType && observed.FieldPath() == fieldPath) {
            return &observed;
        }
    }
    return nullptr;
}

const TObservedEntry& RequireEntry(const TVector<TObservedEntry>& entries,
        TStringBuf opType, TStringBuf fieldPath)
{
    const auto* found = FindEntry(entries, opType, fieldPath);
    if (!found) {
        TStringBuilder dump;
        for (const auto& observed : entries) {
            dump << "\n  " << observed.OpType() << " / " << observed.FieldPath()
                 << " -> " << observed.AbsPath();
        }
        UNIT_FAIL("no footprint entry for " << opType << " / " << fieldPath << ", have:" << dump);
    }
    return *found;
}

const TObservedEntry& RequireEntryByAbsPath(const TVector<TObservedEntry>& entries,
        TStringBuf opType, TStringBuf absPath)
{
    for (const auto& observed : entries) {
        if (observed.OpType() == opType && observed.AbsPath() == absPath) {
            return observed;
        }
    }
    UNIT_FAIL("no footprint entry for " << opType << " at " << absPath);
    return entries.front();
}

TVector<TString> AbsPaths(const TVector<TObservedEntry>& entries,
        TStringBuf opType, TStringBuf fieldPath)
{
    TVector<TString> result;
    for (const auto& observed : entries) {
        if (observed.OpType() == opType && observed.FieldPath() == fieldPath) {
            result.push_back(observed.AbsPath());
        }
    }
    Sort(result);
    return result;
}

////////////////////////////////////////////////////////////////////////////////
// Write set / publication helpers.

const TPathFootprint& RequirePart(const TDeque<TObservedFootprint>& parts,
        TStringBuf opType, size_t from = 0)
{
    for (size_t i = from; i < parts.size(); ++i) {
        if (NKikimrSchemeOp::EOperationType_Name(parts[i].Footprint.PartOpType) == opType) {
            return parts[i].Footprint;
        }
    }
    UNIT_FAIL("no observed part footprint for " << opType);
    return parts.front().Footprint;
}

TPathId PathIdOf(TTestActorRuntime& runtime, const TString& path) {
    // Private paths (index impl tables, cdc stream pq groups) need the private
    // describe.
    const auto& self = DescribePrivatePath(runtime, path).GetPathDescription().GetSelf();
    return TPathId(TOwnerId(self.GetSchemeshardId()), TLocalPathId(self.GetPathId()));
}

// Union of every part's write set, which is what a whole request wrote.
TVector<TPathId> AllWriteSetPathIds(const TDeque<TObservedFootprint>& parts, size_t from = 0) {
    THashSet<TPathId> seen;
    TVector<TPathId> result;
    for (size_t i = from; i < parts.size(); ++i) {
        for (const TPathId& pathId : parts[i].Footprint.WriteSet) {
            if (seen.insert(pathId).second) {
                result.push_back(pathId);
            }
        }
    }
    Sort(result);
    return result;
}

bool Contains(const TVector<TPathId>& haystack, const TPathId& needle) {
    return Find(haystack, needle) != haystack.end();
}

////////////////////////////////////////////////////////////////////////////////
// The DEBUG log rendering of the same footprints, kept alive by one test.

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

////////////////////////////////////////////////////////////////////////////////
// Rewrite helpers.
//
// The layer-3 rewriters take a footprint that ResolvePathFootprint produced on
// the schemeshard that owns the paths. A pure test has no schemeshard, so it
// builds the footprint from the real extractor and fakes only the resolution:
// the field identities, kinds, roles and raw values under test are the
// production ones, and the AbsPath is whatever the test says the path is.

TString JoinAbs(TStringBuf dir, TStringBuf leaf) {
    if (leaf.empty()) {
        return TString(dir);
    }
    if (dir.empty() || dir == "/") {
        return TStringBuilder() << '/' << leaf;
    }
    return TStringBuilder() << dir << '/' << leaf;
}

TString StripDbPrefix(TStringBuf abs, TStringBuf db) {
    if (db.empty() || db == "/") {
        return TString(abs.StartsWith('/') ? abs.substr(1) : abs);
    }
    if (abs == db) {
        return TString();
    }
    if (abs.StartsWith(db) && abs.size() > db.size() && abs[db.size()] == '/') {
        return TString(abs.substr(db.size() + 1));
    }
    return TString(abs);
}

TString LeafOf(TStringBuf abs) {
    const size_t slash = abs.rfind('/');
    return TString(slash == TStringBuf::npos ? abs : abs.substr(slash + 1));
}

// Absolute path of a value resolved the way ResolvePathFootprint would.
// `byId` maps a local path id to the path it stands for; an id missing from it
// is an id that did not resolve.
TPathFootprint FakeResolve(const NKikimrSchemeOp::TModifyScheme& tx, const TString& databasePath,
        const THashMap<ui64, TString>& byId = {})
{
    const TString workingDir = tx.GetWorkingDir();

    TPathFootprint fp;
    fp.WorkingDir = workingDir;
    fp.WorkingDirCanon = workingDir;
    fp.WorkingDirRelToDb = StripDbPrefix(workingDir, databasePath);
    fp.PartOpType = tx.GetOperationType();

    const auto relativeOrAbsolute = [&](TStringBuf value) {
        return value.StartsWith('/') ? TString(value) : JoinAbs(workingDir, value);
    };

    for (const auto& ref : ExtractPathRefs(tx)) {
        TPathFootprintEntry entry;
        entry.Ref.Field = ref.Field;
        entry.Ref.FieldPath = FieldPath(ref);
        entry.Ref.Value = TString(ref.Value);
        entry.Ref.OwnerId = ref.OwnerId;
        entry.Ref.LocalPathId = ref.LocalPathId;
        entry.Ref.Kind = ref.Kind;
        entry.Ref.Role = ref.Role;
        entry.Ref.BasePath = TString(ref.BasePath);
        entry.Ref.AnchorIndex = ref.AnchorIndex;
        entry.Ref.Index = ref.Index;

        const auto anchor = [&]() -> const TPathFootprintEntry* {
            return ref.AnchorIndex >= 0 && size_t(ref.AnchorIndex) < fp.Entries.size()
                ? &fp.Entries[ref.AnchorIndex]
                : nullptr;
        };

        switch (ref.Kind) {
        case EPathRefKind::LeafUnderWorkingDir:
            entry.AbsPath = JoinAbs(workingDir, ref.Value);
            break;
        case EPathRefKind::PathUnderWorkingDirSplit:
            entry.AbsPath = JoinAbs(workingDir,
                ref.Value.StartsWith('/') ? ref.Value.substr(1) : ref.Value);
            break;
        case EPathRefKind::PathUnderWorkingDir:
            entry.AbsPath = ref.Value.empty() ? workingDir : relativeOrAbsolute(ref.Value);
            break;
        case EPathRefKind::Absolute:
            entry.AbsPath = ref.Value.empty() ? workingDir : TString(ref.Value);
            break;
        case EPathRefKind::LeafUnderSibling:
            if (ref.BasePath.empty()) {
                if (const auto* base = anchor(); base && !base->AbsPath.empty()) {
                    entry.AbsPath = JoinAbs(base->AbsPath, ref.Value);
                }
            } else {
                entry.AbsPath = JoinAbs(relativeOrAbsolute(ref.BasePath), ref.Value);
            }
            break;
        case EPathRefKind::ById:
            if (const auto* resolved = byId.FindPtr(ref.LocalPathId)) {
                entry.AbsPath = *resolved;
            }
            break;
        case EPathRefKind::Implicit:
            if (const auto* base = anchor()) {
                entry.AbsPath = base->AbsPath;
            }
            break;
        }

        entry.Exists = !entry.AbsPath.empty();
        entry.RelPathToParent = LeafOf(entry.AbsPath);
        entry.RelPathToDatabase = StripDbPrefix(entry.AbsPath, databasePath);
        entry.RelPathToWorkingDir = StripDbPrefix(entry.AbsPath, workingDir);
        fp.Entries.push_back(std::move(entry));
    }

    return fp;
}

// Sends a hand-built TModifyScheme; helpers.h exports no such entry point.
void SendModify(TTestActorRuntime& runtime, ui64 txId, const NKikimrSchemeOp::TModifyScheme& tx) {
    auto* ev = new TEvTx(txId, TTestTxConfig::SchemeShard);
    *ev->Record.AddTransaction() = tx;
    AsyncSend(runtime, TTestTxConfig::SchemeShard, ev);
    TestModificationResults(runtime, txId, {{NKikimrScheme::StatusAccepted}});
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
        TFootprintCollector collector;
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().PathFootprintObserver(&collector));
        ui64 txId = 100;

        const size_t mark = collector.Parts.size();
        const size_t requestMark = collector.Requests.size();
        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "a/b/Table"
            Columns { Name: "key" Type: "Uint64" }
            Columns { Name: "value" Type: "Uint64" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        const auto entries = Flatten(collector.Parts);

        // Two auto-generated MkDir parts, then the CreateTable part.
        // (/MyRoot/.sys is created by the test env itself.)
        TVector<TString> mkdirs;
        for (const TString& path : AbsPaths(entries, "ESchemeOpMkDir", "MkDir.Name")) {
            if (!path.StartsWith("/MyRoot/.sys")) {
                mkdirs.push_back(path);
            }
        }
        UNIT_ASSERT_VALUES_EQUAL(mkdirs, (TVector<TString>{"/MyRoot/a", "/MyRoot/a/b"}));

        const auto& table = RequireEntry(entries, "ESchemeOpCreateTable", "CreateTable.Name");
        UNIT_ASSERT_VALUES_EQUAL(table.Entry->AbsPath, "/MyRoot/a/b/Table");
        UNIT_ASSERT_VALUES_EQUAL(TString(PathRefKindName(table.Entry->Ref.Kind)), "LeafUnderWorkingDir");
        UNIT_ASSERT_VALUES_EQUAL(TString(PathRefRoleName(table.Entry->Ref.Role)), "Target");
        UNIT_ASSERT_VALUES_EQUAL(table.Entry->Exists, false);  // not created yet at Propose
        UNIT_ASSERT_VALUES_EQUAL(table.Entry->RelPathToDatabase, "a/b/Table");
        UNIT_ASSERT_VALUES_EQUAL(table.Entry->RelPathToWorkingDir, "Table");
        UNIT_ASSERT_VALUES_EQUAL(table.Entry->RelPathToParent, "Table");
        UNIT_ASSERT_VALUES_EQUAL(table.Part->WorkingDirRelToDb, "a/b");
        UNIT_ASSERT_VALUES_EQUAL(
            NKikimrScheme::EStatus_Name(table.Part->ProposeStatus), "StatusAccepted");

        // All three parts descend from the single client transaction.
        const auto ownEntries = Flatten(collector.Parts, mark);
        for (const auto& observed : ownEntries) {
            UNIT_ASSERT_VALUES_EQUAL_C(observed.Part->OriginalTxIndex, 0u,
                observed.OpType() << " / " << observed.FieldPath());
        }

        // ... and there is exactly one request footprint, describing the
        // request as the client wrote it: one multi-segment leaf name, not the
        // three derived parts.
        UNIT_ASSERT_VALUES_EQUAL(collector.Requests.size() - requestMark, 1u);
        const auto& request = collector.Requests[requestMark].Footprint;
        UNIT_ASSERT_VALUES_EQUAL(request.PartId, InvalidSubTxId);
        UNIT_ASSERT_VALUES_EQUAL(request.OriginalTxIndex, 0u);
        UNIT_ASSERT_VALUES_EQUAL(
            NKikimrSchemeOp::EOperationType_Name(request.PartOpType), "ESchemeOpCreateTable");
        UNIT_ASSERT_VALUES_EQUAL(request.Entries.size(), 1u);
        UNIT_ASSERT_VALUES_EQUAL(request.Entries[0].Ref.FieldPath, "CreateTable.Name");
        UNIT_ASSERT_VALUES_EQUAL(
            TString(PathRefKindName(request.Entries[0].Ref.Kind)), "LeafUnderWorkingDir");
        UNIT_ASSERT_VALUES_EQUAL(request.Entries[0].RelPathToDatabase, "a/b/Table");
        UNIT_ASSERT_VALUES_EQUAL(request.WorkingDir, "/MyRoot");

        // The MkDir parts go through TMemoryChanges, so their write set is
        // exact: each new directory plus the parent whose child list changed.
        const TVector<TPathId> written = AllWriteSetPathIds(collector.Parts, mark);
        for (const TString& path : {TString("/MyRoot"), TString("/MyRoot/a"), TString("/MyRoot/a/b")}) {
            UNIT_ASSERT_C(Contains(written, PathIdOf(runtime, path)),
                "write set has no " << path);
        }

        // TCreateTable::Propose writes straight through context.GetDB()
        // instead of recording TMemoryChanges, so its own write set is empty
        // and the part is flagged as a lower bound. The new table id is
        // therefore *not* in the write set above.
        const auto& createPart = RequirePart(collector.Parts, "ESchemeOpCreateTable", mark);
        UNIT_ASSERT_VALUES_EQUAL(createPart.WriteSet.size(), 0u);
        UNIT_ASSERT_VALUES_EQUAL(createPart.WriteSetMayBeIncomplete, true);
        UNIT_ASSERT_VALUES_EQUAL(Contains(written, PathIdOf(runtime, "/MyRoot/a/b/Table")), false);

        // The MkDir parts ran before any direct db write, so they are exact.
        const auto& mkdirPart = RequirePart(collector.Parts, "ESchemeOpMkDir", mark);
        UNIT_ASSERT_VALUES_EQUAL(mkdirPart.WriteSetMayBeIncomplete, false);
        UNIT_ASSERT_VALUES_EQUAL(mkdirPart.WriteSet.size(), 2u);
        UNIT_ASSERT(!mkdirPart.Published.empty());
    }

    // The observer replaced the log as the test channel, but the DEBUG line is
    // still the production rendering: it must keep rendering the same fields
    // once FLAT_TX_SCHEMESHARD admits DEBUG.
    Y_UNIT_TEST(DebugLogStillRendersTheFootprint) {
        TVector<TString> log;
        TTestBasicRuntime runtime;
        runtime.SetLogBackend(new TLogRecordCollector(&log));
        TTestEnv env(runtime);
        // TTestEnv already raises FLAT_TX_SCHEMESHARD to DEBUG; pinned here so
        // the test does not silently depend on that default.
        runtime.SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_DEBUG);
        ui64 txId = 100;

        const size_t mark = log.size();
        TestMkDir(runtime, ++txId, "/MyRoot", "LoggedDir");
        env.TestWaitNotification(runtime, txId);

        bool foundPart = false;
        bool foundRequest = false;
        bool foundWriteSet = false;
        for (size_t i = mark; i < log.size(); ++i) {
            const TStringBuf line = log[i];
            if (line.find("PathFootprint") == TStringBuf::npos) {
                continue;
            }
            if (line.find("absPath# /MyRoot/LoggedDir") == TStringBuf::npos) {
                foundWriteSet = foundWriteSet
                    || line.find("fieldPath# <writeSet>") != TStringBuf::npos;
                continue;
            }
            UNIT_ASSERT_C(line.find("fieldPath# MkDir.Name") != TStringBuf::npos, line);
            UNIT_ASSERT_C(line.find("partOpType# ESchemeOpMkDir") != TStringBuf::npos, line);
            if (line.find("PathFootprint request") != TStringBuf::npos) {
                foundRequest = true;
            } else {
                foundPart = true;
            }
        }
        UNIT_ASSERT_C(foundPart, "no part-level PathFootprint DEBUG line");
        UNIT_ASSERT_C(foundRequest, "no request-level PathFootprint DEBUG line");
        UNIT_ASSERT_C(foundWriteSet, "no write set PathFootprint DEBUG line");
    }

    // ... and with neither an observer nor DEBUG logging, nothing is computed
    // at all. TTestEnv::SetupLogging leaves FLAT_TX_SCHEMESHARD at DEBUG
    // (ENABLE_SCHEMESHARD_LOG defaults to true), so the production default has
    // to be asked for explicitly.
    Y_UNIT_TEST(NoObserverAndNoDebugLogMeansNoFootprint) {
        TVector<TString> log;
        TTestBasicRuntime runtime;
        runtime.SetLogBackend(new TLogRecordCollector(&log));
        TTestEnv env(runtime);
        runtime.SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_NOTICE);
        ui64 txId = 100;

        const size_t mark = log.size();
        TestMkDir(runtime, ++txId, "/MyRoot", "QuietDir");
        env.TestWaitNotification(runtime, txId);

        for (size_t i = mark; i < log.size(); ++i) {
            UNIT_ASSERT_C(TStringBuf(log[i]).find("PathFootprint") == TStringBuf::npos, log[i]);
        }
    }

    Y_UNIT_TEST(CreateIndexedTable) {
        TFootprintCollector collector;
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().PathFootprintObserver(&collector));
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

        const auto entries = Flatten(collector.Parts);

        // The client request itself is not a part; the parts are the derived
        // CreateTable / CreateTableIndex / CreateTable(implTable) protos, each
        // already carrying an absolute WorkingDir and a leaf Name.
        UNIT_ASSERT_VALUES_EQUAL(
            AbsPaths(entries, "ESchemeOpCreateTable", "CreateTable.Name"),
            (TVector<TString>{"/MyRoot/Table", "/MyRoot/Table/byValue/indexImplTable"}));

        const auto& index = RequireEntry(entries, "ESchemeOpCreateTableIndex", "CreateTableIndex.Name");
        UNIT_ASSERT_VALUES_EQUAL(index.Entry->AbsPath, "/MyRoot/Table/byValue");
        UNIT_ASSERT_VALUES_EQUAL(index.Entry->RelPathToDatabase, "Table/byValue");

        const auto& impl = RequireEntryByAbsPath(entries,
            "ESchemeOpCreateTable", "/MyRoot/Table/byValue/indexImplTable");
        UNIT_ASSERT_VALUES_EQUAL(impl.Entry->RelPathToWorkingDir, "indexImplTable");
        UNIT_ASSERT_VALUES_EQUAL(impl.Part->WorkingDirRelToDb, "Table/byValue");
    }

    Y_UNIT_TEST(CreateCdcStream) {
        TFootprintCollector collector;
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions()
            .EnableProtoSourceIdInfo(true)
            .PathFootprintObserver(&collector));
        ui64 txId = 100;

        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "Table"
            Columns { Name: "key" Type: "Uint64" }
            Columns { Name: "value" Type: "Uint64" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        const size_t mark = collector.Parts.size();
        TestCreateCdcStream(runtime, ++txId, "/MyRoot", R"(
            TableName: "Table"
            StreamDescription {
              Name: "Stream"
              Mode: ECdcStreamModeKeysOnly
              Format: ECdcStreamFormatProto
            }
        )");
        env.TestWaitNotification(runtime, txId);

        const auto entries = Flatten(collector.Parts, mark);

        const auto& atTable = RequireEntry(entries,
            "ESchemeOpCreateCdcStreamAtTable", "CreateCdcStream.TableName");
        UNIT_ASSERT_VALUES_EQUAL(atTable.Entry->AbsPath, "/MyRoot/Table");
        UNIT_ASSERT_VALUES_EQUAL(atTable.Entry->Exists, true);
        UNIT_ASSERT(atTable.Entry->PathId);

        const auto& impl = RequireEntry(entries,
            "ESchemeOpCreateCdcStreamImpl", "CreateCdcStream.StreamDescription.Name");
        UNIT_ASSERT_VALUES_EQUAL(impl.Entry->AbsPath, "/MyRoot/Table/Stream");
        UNIT_ASSERT_VALUES_EQUAL(impl.Entry->RelPathToDatabase, "Table/Stream");

        // The AtTable part resolves the stream leaf too (it fills
        // txState.CdcPathId from it), so the footprint must report it.
        const auto& atTableStream = RequireEntry(entries,
            "ESchemeOpCreateCdcStreamAtTable", "CreateCdcStream.StreamDescription.Name");
        UNIT_ASSERT_VALUES_EQUAL(atTableStream.Entry->AbsPath, "/MyRoot/Table/Stream");
        UNIT_ASSERT_VALUES_EQUAL(
            TString(PathRefKindName(atTableStream.Entry->Ref.Kind)), "LeafUnderSibling");
        UNIT_ASSERT_VALUES_EQUAL(atTableStream.Entry->RelPathToWorkingDir, "Table/Stream");
    }

    Y_UNIT_TEST(MoveTable) {
        TFootprintCollector collector;
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().PathFootprintObserver(&collector));
        ui64 txId = 100;

        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "Table"
            Columns { Name: "key" Type: "Uint64" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        const size_t mark = collector.Parts.size();
        TestMoveTable(runtime, ++txId, "/MyRoot/Table", "/MyRoot/Moved");
        env.TestWaitNotification(runtime, txId);

        const auto entries = Flatten(collector.Parts, mark);

        const auto& src = RequireEntry(entries, "ESchemeOpMoveTable", "MoveTable.SrcPath");
        UNIT_ASSERT_VALUES_EQUAL(src.Entry->AbsPath, "/MyRoot/Table");
        UNIT_ASSERT_VALUES_EQUAL(TString(PathRefRoleName(src.Entry->Ref.Role)), "Source");
        UNIT_ASSERT_VALUES_EQUAL(src.Entry->Exists, true);

        const auto& dst = RequireEntry(entries, "ESchemeOpMoveTable", "MoveTable.DstPath");
        UNIT_ASSERT_VALUES_EQUAL(dst.Entry->AbsPath, "/MyRoot/Moved");
        UNIT_ASSERT_VALUES_EQUAL(TString(PathRefRoleName(dst.Entry->Ref.Role)), "Target");
        UNIT_ASSERT_VALUES_EQUAL(dst.Entry->Exists, false);
    }

    // The Move* parts resolve their destination with
    // TPath::ResolveWithInactive, because the destination's parent may be held
    // by an earlier part of the same transaction. The part-level footprint
    // resolves it the same way, so it never reports a path the operation will
    // not use; the request-level footprint has no part to walk back from and
    // stays on the plain resolver.
    Y_UNIT_TEST(MoveIndexedTableResolvesEveryMoveDestination) {
        TFootprintCollector collector;
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().PathFootprintObserver(&collector));
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

        const size_t mark = collector.Parts.size();
        const size_t requestMark = collector.Requests.size();
        TestMoveTable(runtime, ++txId, "/MyRoot/Table", "/MyRoot/Moved");
        env.TestWaitNotification(runtime, txId);

        const auto entries = Flatten(collector.Parts, mark);

        // The MoveTable part: destination resolved through the inactive-aware
        // resolver, same answer as the operation's own dstPath.
        const auto& tableDst = RequireEntry(entries, "ESchemeOpMoveTable", "MoveTable.DstPath");
        UNIT_ASSERT_VALUES_EQUAL(tableDst.Entry->AbsPath, "/MyRoot/Moved");
        UNIT_ASSERT_VALUES_EQUAL(TString(PathRefRoleName(tableDst.Entry->Ref.Role)), "Target");

        // The source keeps the plain resolver: it is the still-active original.
        const auto& tableSrc = RequireEntry(entries, "ESchemeOpMoveTable", "MoveTable.SrcPath");
        UNIT_ASSERT_VALUES_EQUAL(tableSrc.Entry->AbsPath, "/MyRoot/Table");
        UNIT_ASSERT_VALUES_EQUAL(tableSrc.Entry->Exists, true);

        // The derived MoveTableIndex part hangs its destination off a table
        // that only the earlier MoveTable part of this transaction created.
        const auto& indexDst = RequireEntry(entries,
            "ESchemeOpMoveTableIndex", "MoveTableIndex.DstPath");
        UNIT_ASSERT_VALUES_EQUAL(indexDst.Entry->AbsPath, "/MyRoot/Moved/byValue");
        const auto& indexSrc = RequireEntry(entries,
            "ESchemeOpMoveTableIndex", "MoveTableIndex.SrcPath");
        UNIT_ASSERT_VALUES_EQUAL(indexSrc.Entry->AbsPath, "/MyRoot/Table/byValue");

        // The request footprint is resolved before any part exists, so it uses
        // the plain resolver and still describes the request as submitted.
        UNIT_ASSERT_VALUES_EQUAL(collector.Requests.size() - requestMark, 1u);
        const TPathFootprint& request = collector.Requests[requestMark].Footprint;
        UNIT_ASSERT_VALUES_EQUAL(request.PartId, InvalidSubTxId);
        bool sawDst = false;
        for (const auto& entry : request.Entries) {
            if (entry.Ref.FieldPath == "MoveTable.DstPath") {
                UNIT_ASSERT_VALUES_EQUAL(entry.AbsPath, "/MyRoot/Moved");
                sawDst = true;
            }
        }
        UNIT_ASSERT_C(sawDst, "no MoveTable.DstPath in the request footprint");
    }

    Y_UNIT_TEST(DropTableByNameAndById) {
        TFootprintCollector collector;
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().PathFootprintObserver(&collector));
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

        size_t mark = collector.Parts.size();
        TestDropTable(runtime, ++txId, "/MyRoot", "ByName");
        env.TestWaitNotification(runtime, txId);
        {
            const auto entries = Flatten(collector.Parts, mark);
            const auto& drop = RequireEntry(entries, "ESchemeOpDropTable", "Drop.Name");
            UNIT_ASSERT_VALUES_EQUAL(drop.Entry->AbsPath, "/MyRoot/ByName");
            UNIT_ASSERT_VALUES_EQUAL(
                TString(PathRefKindName(drop.Entry->Ref.Kind)), "LeafUnderWorkingDir");
            UNIT_ASSERT_VALUES_EQUAL(drop.Entry->Exists, true);
        }

        mark = collector.Parts.size();
        TestDropTable(runtime, ++txId, localPathId);
        env.TestWaitNotification(runtime, txId);
        {
            const auto entries = Flatten(collector.Parts, mark);
            const auto& drop = RequireEntry(entries, "ESchemeOpDropTable", "Drop.Id");
            UNIT_ASSERT_VALUES_EQUAL(TString(PathRefKindName(drop.Entry->Ref.Kind)), "ById");
            UNIT_ASSERT_VALUES_EQUAL(drop.Entry->AbsPath, "/MyRoot/ById");
            UNIT_ASSERT_VALUES_EQUAL(drop.Entry->Exists, true);
        }
    }

    Y_UNIT_TEST(ConsistentCopyTables) {
        TFootprintCollector collector;
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().PathFootprintObserver(&collector));
        ui64 txId = 100;

        for (int i = 0; i < 2; ++i) {
            TestCreateTable(runtime, ++txId, "/MyRoot", Sprintf(R"(
                Name: "Src%d"
                Columns { Name: "key" Type: "Uint64" }
                KeyColumnNames: ["key"]
            )", i));
            env.TestWaitNotification(runtime, txId);
        }

        const size_t mark = collector.Parts.size();
        TestConsistentCopyTables(runtime, ++txId, "/MyRoot", R"(
            CopyTableDescriptions { SrcPath: "/MyRoot/Src0" DstPath: "/MyRoot/Dst0" }
            CopyTableDescriptions { SrcPath: "/MyRoot/Src1" DstPath: "/MyRoot/Dst1" }
        )");
        env.TestWaitNotification(runtime, txId);

        // Each item becomes its own CreateTable part with an absolute
        // WorkingDir and a leaf Name; both destinations must be present.
        const auto entries = Flatten(collector.Parts, mark);
        UNIT_ASSERT_VALUES_EQUAL(
            AbsPaths(entries, "ESchemeOpCreateTable", "CreateTable.Name"),
            (TVector<TString>{"/MyRoot/Dst0", "/MyRoot/Dst1"}));
    }

    // A backup collection's ExplicitEntryList entries are an Absolute field:
    // RegisterBackupCollectionTables() resolves each with TPath::Resolve() and
    // never joins WorkingDir (schemeshard_impl.cpp:3920). Layer 2 must do the
    // same even when the value has no leading slash, otherwise it invents a
    // path under the working dir that the operation never touches.
    Y_UNIT_TEST(BackupCollectionEntriesAreAbsoluteNotWorkingDirRelative) {
        TFootprintCollector collector;
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions()
            .EnableBackupService(true)
            .PathFootprintObserver(&collector));
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

        const size_t mark = collector.Parts.size();
        TestCreateBackupCollection(runtime, ++txId, "/MyRoot/.backups/collections/", R"(
            Name: "MyCollection"
            ExplicitEntryList {
                Entries { Type: ETypeTable Path: "/MyRoot/Table1" }
                Entries { Type: ETypeTable Path: "Table1" }
            }
            Cluster: {}
        )");
        env.TestWaitNotification(runtime, txId);

        const auto entries = Flatten(collector.Parts, mark);

        const auto& absolute = RequireEntry(entries, "ESchemeOpCreateBackupCollection",
            "CreateBackupCollection.ExplicitEntryList.Entries[0].Path");
        UNIT_ASSERT_VALUES_EQUAL(TString(PathRefKindName(absolute.Entry->Ref.Kind)), "Absolute");
        UNIT_ASSERT_VALUES_EQUAL(TString(PathRefRoleName(absolute.Entry->Ref.Role)), "Dependency");
        UNIT_ASSERT_VALUES_EQUAL(absolute.Entry->AbsPath, "/MyRoot/Table1");
        UNIT_ASSERT_VALUES_EQUAL(absolute.Entry->Exists, true);

        // No leading slash, but still not joined with the working dir.
        const auto& relative = RequireEntry(entries, "ESchemeOpCreateBackupCollection",
            "CreateBackupCollection.ExplicitEntryList.Entries[1].Path");
        UNIT_ASSERT_VALUES_EQUAL(relative.Entry->AbsPath, "/Table1");
        UNIT_ASSERT_VALUES_EQUAL(relative.Entry->Exists, false);
    }

    Y_UNIT_TEST(RejectedCreateTableStillProducesFootprint) {
        TFootprintCollector collector;
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().PathFootprintObserver(&collector));
        ui64 txId = 100;

        const size_t mark = collector.Parts.size();
        TestCreateTable(runtime, ++txId, "/MyRoot/NoSuchDir", R"(
            Name: "Table"
            Columns { Name: "key" Type: "Uint64" }
            KeyColumnNames: ["key"]
        )", {NKikimrScheme::StatusPathDoesNotExist});

        const auto entries = Flatten(collector.Parts, mark);
        const auto& table = RequireEntry(entries, "ESchemeOpCreateTable", "CreateTable.Name");
        UNIT_ASSERT_VALUES_EQUAL(table.Entry->AbsPath, "/MyRoot/NoSuchDir/Table");
        UNIT_ASSERT_VALUES_EQUAL(table.Entry->Exists, false);
        UNIT_ASSERT_VALUES_EQUAL(
            NKikimrScheme::EStatus_Name(table.Part->ProposeStatus), "StatusPathDoesNotExist");
        // Best effort even though nothing under the working dir resolves.
        UNIT_ASSERT_VALUES_EQUAL(table.Entry->RelPathToDatabase, "NoSuchDir/Table");

        // A part that fails its checks never gets as far as writing anything.
        const auto& part = RequirePart(collector.Parts, "ESchemeOpCreateTable", mark);
        UNIT_ASSERT_VALUES_EQUAL(part.WriteSet.size(), 0u);
        UNIT_ASSERT_VALUES_EQUAL(part.Published.size(), 0u);
        UNIT_ASSERT_VALUES_EQUAL(part.WriteSetMayBeIncomplete, false);
        UNIT_ASSERT_VALUES_EQUAL(AllWriteSetPathIds(collector.Parts, mark), (TVector<TPathId>{}));
    }

    // Dropping an indexed table names only the table, but the operation
    // touches the index and its impl table too. Those cascaded paths appear in
    // the write set although no proto field of the request mentions them.
    Y_UNIT_TEST(DropIndexedTableWriteSetCoversTheCascade) {
        TFootprintCollector collector;
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().PathFootprintObserver(&collector));
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

        const TVector<std::pair<TString, TPathId>> expected = {
            {"/MyRoot", PathIdOf(runtime, "/MyRoot")},
            {"/MyRoot/Table", PathIdOf(runtime, "/MyRoot/Table")},
            {"/MyRoot/Table/byValue", PathIdOf(runtime, "/MyRoot/Table/byValue")},
            {"/MyRoot/Table/byValue/indexImplTable",
                PathIdOf(runtime, "/MyRoot/Table/byValue/indexImplTable")},
        };

        const size_t mark = collector.Parts.size();
        TestDropTable(runtime, ++txId, "/MyRoot", "Table");
        env.TestWaitNotification(runtime, txId);

        const TVector<TPathId> written = AllWriteSetPathIds(collector.Parts, mark);
        for (const auto& [name, pathId] : expected) {
            UNIT_ASSERT_C(Contains(written, pathId), "write set has no " << name);
        }
    }

    // Two transactions in one request: every part carries the index of the
    // client transaction it descends from, and each gets its own request
    // footprint.
    Y_UNIT_TEST(TwoTransactionsGetDistinctOriginalTxIndexes) {
        TFootprintCollector collector;
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().PathFootprintObserver(&collector));
        ui64 txId = 100;

        const size_t mark = collector.Parts.size();
        const size_t requestMark = collector.Requests.size();
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

        UNIT_ASSERT_VALUES_EQUAL(collector.Requests.size() - requestMark, 2u);
        const auto& first = collector.Requests[requestMark].Footprint;
        const auto& second = collector.Requests[requestMark + 1].Footprint;
        UNIT_ASSERT_VALUES_EQUAL(first.OriginalTxIndex, 0u);
        UNIT_ASSERT_VALUES_EQUAL(first.Entries.size(), 1u);
        UNIT_ASSERT_VALUES_EQUAL(first.Entries[0].AbsPath, "/MyRoot/first");
        UNIT_ASSERT_VALUES_EQUAL(second.OriginalTxIndex, 1u);
        UNIT_ASSERT_VALUES_EQUAL(second.Entries.size(), 1u);
        UNIT_ASSERT_VALUES_EQUAL(second.Entries[0].AbsPath, "/MyRoot/second/nested");

        // The second transaction fans out into a generated MkDir for "second"
        // plus the MkDir for "nested"; all of them point back at index 1.
        THashMap<TString, ui32> originByAbsPath;
        for (const auto& observed : Flatten(collector.Parts, mark)) {
            if (observed.FieldPath() == "MkDir.Name") {
                originByAbsPath[observed.AbsPath()] = observed.Part->OriginalTxIndex;
            }
        }
        UNIT_ASSERT_VALUES_EQUAL(originByAbsPath["/MyRoot/first"], 0u);
        UNIT_ASSERT_VALUES_EQUAL(originByAbsPath["/MyRoot/second"], 1u);
        UNIT_ASSERT_VALUES_EQUAL(originByAbsPath["/MyRoot/second/nested"], 1u);
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

// Layer 3: canonicalizing a by-id request into a by-name one, and moving every
// path a request spells out into another database.
Y_UNIT_TEST_SUITE(TSchemeShardPathFootprintRewrite) {

    Y_UNIT_TEST(CanonicalizeDropTableById) {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpDropTable, "/MyRoot");
        tx.MutableDrop()->SetId(42);
        const auto fp = FakeResolve(tx, "/MyRoot", {{42, "/MyRoot/Dir/T"}});

        auto copy = tx;
        const auto result = CanonicalizeToPaths(copy, fp);

        UNIT_ASSERT(result.Changed);
        UNIT_ASSERT(result.Untransformable.empty());
        UNIT_ASSERT_VALUES_EQUAL(copy.GetWorkingDir(), "/MyRoot/Dir");
        UNIT_ASSERT_VALUES_EQUAL(copy.GetDrop().GetName(), "T");
        UNIT_ASSERT_C(!copy.GetDrop().HasId(), "the id must go: rmdir.cpp:32 takes it over the name");
        // The request the footprint was resolved from is never touched.
        UNIT_ASSERT(tx.GetDrop().HasId());
    }

    Y_UNIT_TEST(CanonicalizeAlterTableByPathIdClearsBothIdForms) {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpAlterTable, "/MyRoot");
        TPathId(TOwnerId(72057594046678944ull), TLocalPathId(7)).ToProto(
            tx.MutableAlterTable()->MutablePathId());
        tx.MutableAlterTable()->SetId_Deprecated(7);
        auto* column = tx.MutableAlterTable()->AddColumns();
        column->SetName("added");
        column->SetType("Uint64");
        const auto fp = FakeResolve(tx, "/MyRoot", {{7, "/MyRoot/Dir/T"}});

        auto copy = tx;
        const auto result = CanonicalizeToPaths(copy, fp);

        UNIT_ASSERT(result.Changed);
        UNIT_ASSERT_VALUES_EQUAL(copy.GetWorkingDir(), "/MyRoot/Dir");
        UNIT_ASSERT_VALUES_EQUAL(copy.GetAlterTable().GetName(), "T");
        UNIT_ASSERT(!copy.GetAlterTable().HasPathId());
        UNIT_ASSERT(!copy.GetAlterTable().HasId_Deprecated());
        // Everything else survives untouched.
        UNIT_ASSERT_VALUES_EQUAL(copy.GetAlterTable().ColumnsSize(), 1u);
        UNIT_ASSERT_VALUES_EQUAL(copy.GetAlterTable().GetColumns(0).GetName(), "added");
    }

    Y_UNIT_TEST(CanonicalizeSplitMergeKeepsTheAbsoluteTablePath) {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpSplitMergeTablePartitions, "/MyRoot");
        auto& info = *tx.MutableSplitMergeTablePartitions();
        info.SetTableOwnerId(72057594046678944ull);
        info.SetTableLocalId(11);
        const auto fp = FakeResolve(tx, "/MyRoot", {{11, "/MyRoot/Dir/T"}});

        auto copy = tx;
        const auto result = CanonicalizeToPaths(copy, fp);

        UNIT_ASSERT(result.Changed);
        // split_merge.cpp:849 resolves TablePath absolutely, WorkingDir is not
        // consulted and must not be rewritten.
        UNIT_ASSERT_VALUES_EQUAL(copy.GetSplitMergeTablePartitions().GetTablePath(), "/MyRoot/Dir/T");
        UNIT_ASSERT_VALUES_EQUAL(copy.GetWorkingDir(), "/MyRoot");
        UNIT_ASSERT(!copy.GetSplitMergeTablePartitions().HasTableOwnerId());
        UNIT_ASSERT(!copy.GetSplitMergeTablePartitions().HasTableLocalId());
    }

    Y_UNIT_TEST(CanonicalizeUnknownIdIsUntransformable) {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpDropTable, "/MyRoot");
        tx.MutableDrop()->SetId(42);
        // Nothing resolves the id: it names a path this schemeshard does not have.
        const auto fp = FakeResolve(tx, "/MyRoot");

        auto copy = tx;
        const auto result = CanonicalizeToPaths(copy, fp);

        UNIT_ASSERT(!result.Changed);
        UNIT_ASSERT_VALUES_EQUAL(result.Untransformable.size(), 1u);
        UNIT_ASSERT_EQUAL(result.Untransformable[0], EPathField::Drop_Id);
        UNIT_ASSERT_VALUES_EQUAL(copy.DebugString(), tx.DebugString());
    }

    Y_UNIT_TEST(CanonicalizeLeavesAByNameRequestAlone) {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpDropTable, "/MyRoot/Dir");
        tx.MutableDrop()->SetName("T");
        const auto fp = FakeResolve(tx, "/MyRoot");

        auto copy = tx;
        const auto result = CanonicalizeToPaths(copy, fp);

        UNIT_ASSERT(!result.Changed);
        UNIT_ASSERT(result.Untransformable.empty());
        UNIT_ASSERT_VALUES_EQUAL(copy.DebugString(), tx.DebugString());
    }

    Y_UNIT_TEST(RelocateMoveTableRewritesBothPaths) {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpMoveTable, "/MyRoot/db1");
        tx.MutableMoveTable()->SetSrcPath("/MyRoot/db1/a");
        tx.MutableMoveTable()->SetDstPath("/MyRoot/db1/b");
        const auto fp = FakeResolve(tx, "/MyRoot/db1");

        const auto result = RelocatePaths(tx, fp, {"/MyRoot/db1", "/MyRoot2/x/db2"});

        UNIT_ASSERT(result.Changed);
        UNIT_ASSERT(result.Skipped.empty());
        UNIT_ASSERT_VALUES_EQUAL(tx.GetMoveTable().GetSrcPath(), "/MyRoot2/x/db2/a");
        UNIT_ASSERT_VALUES_EQUAL(tx.GetMoveTable().GetDstPath(), "/MyRoot2/x/db2/b");
        UNIT_ASSERT_VALUES_EQUAL(tx.GetWorkingDir(), "/MyRoot2/x/db2");
    }

    Y_UNIT_TEST(RelocateCreateTableTouchesOnlyTheWorkingDir) {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpCreateTable, "/MyRoot/db1/dir");
        tx.MutableCreateTable()->SetName("t");
        const auto fp = FakeResolve(tx, "/MyRoot/db1");

        const auto result = RelocatePaths(tx, fp, {"/MyRoot/db1", "/MyRoot2/x/db2"});

        UNIT_ASSERT(result.Changed);
        UNIT_ASSERT_VALUES_EQUAL(tx.GetWorkingDir(), "/MyRoot2/x/db2/dir");
        // A leaf name is relative to the working dir and moves with it.
        UNIT_ASSERT_VALUES_EQUAL(tx.GetCreateTable().GetName(), "t");
    }

    Y_UNIT_TEST(RelocateConsistentCopyTablesRewritesEveryItem) {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpCreateConsistentCopyTables, "/MyRoot/db1");
        for (int i = 0; i < 2; ++i) {
            auto* item = tx.MutableCreateConsistentCopyTables()->AddCopyTableDescriptions();
            item->SetSrcPath(Sprintf("/MyRoot/db1/src%d", i));
            item->SetDstPath(Sprintf("/MyRoot/db1/dir/dst%d", i));
        }
        const auto fp = FakeResolve(tx, "/MyRoot/db1");

        const auto result = RelocatePaths(tx, fp, {"/MyRoot/db1", "/MyRoot2/x/db2"});

        UNIT_ASSERT(result.Changed);
        const auto& copied = tx.GetCreateConsistentCopyTables();
        UNIT_ASSERT_VALUES_EQUAL(copied.GetCopyTableDescriptions(0).GetSrcPath(), "/MyRoot2/x/db2/src0");
        UNIT_ASSERT_VALUES_EQUAL(copied.GetCopyTableDescriptions(0).GetDstPath(), "/MyRoot2/x/db2/dir/dst0");
        UNIT_ASSERT_VALUES_EQUAL(copied.GetCopyTableDescriptions(1).GetSrcPath(), "/MyRoot2/x/db2/src1");
        UNIT_ASSERT_VALUES_EQUAL(copied.GetCopyTableDescriptions(1).GetDstPath(), "/MyRoot2/x/db2/dir/dst1");
    }

    Y_UNIT_TEST(RelocateRewritesAPathUnderWorkingDirOnlyWhenItIsAbsolute) {
        const auto relocate = [](const TString& pathName) {
            auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpAlterUserAttributes, "/MyRoot/db1/dir");
            tx.MutableAlterUserAttributes()->SetPathName(pathName);
            const auto fp = FakeResolve(tx, "/MyRoot/db1");
            RelocatePaths(tx, fp, {"/MyRoot/db1", "/MyRoot2/x/db2"});
            return tx.GetAlterUserAttributes().GetPathName();
        };

        UNIT_ASSERT_VALUES_EQUAL(relocate("/MyRoot/db1/dir/sub"), "/MyRoot2/x/db2/dir/sub");
        // Relative means "under the working dir", which the working-dir rewrite
        // already moved.
        UNIT_ASSERT_VALUES_EQUAL(relocate("sub"), "sub");
    }

    Y_UNIT_TEST(RelocateLeavesPathsOutsideTheDatabaseAlone) {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpMoveTable, "/MyRoot/other");
        tx.MutableMoveTable()->SetSrcPath("/MyRoot/other/a");
        tx.MutableMoveTable()->SetDstPath("/MyRoot/other/b");
        const auto before = tx.DebugString();
        const auto fp = FakeResolve(tx, "/MyRoot/other");

        const auto result = RelocatePaths(tx, fp, {"/MyRoot/db1", "/MyRoot2/x/db2"});

        UNIT_ASSERT(!result.Changed);
        UNIT_ASSERT_VALUES_EQUAL(tx.DebugString(), before);
    }

    Y_UNIT_TEST(RelocateNeverTouchesAReplicationSourcePath) {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpCreateTransfer, "/MyRoot/db1");
        auto& desc = *tx.MutableReplication();
        desc.SetName("transfer");
        auto& target = *desc.MutableConfig()->MutableTransferSpecific()->MutableTarget();
        // A path on the *remote* cluster. It is never extracted and must never
        // be rewritten, however much it looks like a local path.
        target.SetSrcPath("/MyRoot/db1/remote");
        target.SetDstPath("/MyRoot/db1/local");
        const auto fp = FakeResolve(tx, "/MyRoot/db1");

        const auto result = RelocatePaths(tx, fp, {"/MyRoot/db1", "/MyRoot2/x/db2"});

        UNIT_ASSERT(result.Changed);
        const auto& moved = tx.GetReplication().GetConfig().GetTransferSpecific().GetTarget();
        UNIT_ASSERT_VALUES_EQUAL(moved.GetDstPath(), "/MyRoot2/x/db2/local");
        UNIT_ASSERT_VALUES_EQUAL(moved.GetSrcPath(), "/MyRoot/db1/remote");
    }

    Y_UNIT_TEST(RelocateSkipsAByIdRequest) {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpDropTable, "/MyRoot/db1");
        tx.MutableDrop()->SetId(42);
        const auto fp = FakeResolve(tx, "/MyRoot/db1", {{42, "/MyRoot/db1/dir/T"}});

        const auto result = RelocatePaths(tx, fp, {"/MyRoot/db1", "/MyRoot2/x/db2"});

        UNIT_ASSERT_VALUES_EQUAL(result.Skipped.size(), 1u);
        UNIT_ASSERT_EQUAL(result.Skipped[0], EPathField::Drop_Id);
        UNIT_ASSERT_VALUES_EQUAL(tx.GetDrop().GetId(), 42u);

        // Canonicalizing first is what makes the request relocatable: the drop
        // then names a working dir and a leaf, and the working dir moves.
        auto canonical = tx;
        CanonicalizeToPaths(canonical, fp);
        const auto canonicalFp = FakeResolve(canonical, "/MyRoot/db1");
        const auto second = RelocatePaths(canonical, canonicalFp, {"/MyRoot/db1", "/MyRoot2/x/db2"});
        UNIT_ASSERT(second.Skipped.empty());
        UNIT_ASSERT_VALUES_EQUAL(canonical.GetWorkingDir(), "/MyRoot2/x/db2/dir");
        UNIT_ASSERT_VALUES_EQUAL(canonical.GetDrop().GetName(), "T");
    }

    // The kind a field resolves with can depend on the operation carrying it
    // (finding D7). The footprint records the kind that Propose() actually
    // used, so the rewriter reads it from the entry and never has to key on the
    // operation type itself.
    Y_UNIT_TEST(RelocateFollowsThePerOperationKindOfTheSameField) {
        const auto relocate = [](NKikimrSchemeOp::EOperationType type) {
            auto tx = MakeTx(type, "/MyRoot/db1/dir");
            tx.MutableDropCdcStream()->SetTableName("/MyRoot/db1/dir/T");
            tx.MutableDropCdcStream()->AddStreamName("stream");
            const auto fp = FakeResolve(tx, "/MyRoot/db1");
            RelocatePaths(tx, fp, {"/MyRoot/db1", "/MyRoot2/x/db2"});
            return tx.GetDropCdcStream().GetTableName();
        };

        // Top level: PathUnderWorkingDir, and the value is absolute.
        UNIT_ASSERT_VALUES_EQUAL(relocate(NKikimrSchemeOp::ESchemeOpDropCdcStream),
            "/MyRoot2/x/db2/dir/T");
        // The AtTable part resolves the same field with a plain Dive, so the
        // value is a leaf and rides on the working dir instead.
        UNIT_ASSERT_VALUES_EQUAL(relocate(NKikimrSchemeOp::ESchemeOpDropCdcStreamAtTable),
            "/MyRoot/db1/dir/T");
    }

    Y_UNIT_TEST(RelocateNeverRewritesASplitChild) {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpAlterContinuousBackup, "/MyRoot/db1");
        // A leading slash does not escape the working dir for a split child:
        // TPath::Child(value, TSplitChildTag{}) dives it segment by segment.
        tx.MutableAlterContinuousBackup()->SetTableName("/MyRoot/db1/T");
        const auto fp = FakeResolve(tx, "/MyRoot/db1");

        RelocatePaths(tx, fp, {"/MyRoot/db1", "/MyRoot2/x/db2"});

        UNIT_ASSERT_VALUES_EQUAL(tx.GetAlterContinuousBackup().GetTableName(), "/MyRoot/db1/T");
        UNIT_ASSERT_VALUES_EQUAL(tx.GetWorkingDir(), "/MyRoot2/x/db2");
    }

    // Every field that can carry a relocatable path needs a setter, and nothing
    // else may have one: a setter on a leaf-name field would double-apply the
    // working-dir move.
    Y_UNIT_TEST(EveryRelocatableFieldHasASetter) {
        TVector<TString> missing;
        TVector<TString> unexpected;
        size_t setters = 0;
        for (size_t i = 0; i < size_t(EPathField::Count); ++i) {
            const auto field = static_cast<EPathField>(i);
            const TString name(PathFieldName(field));
            const auto kind = PathFieldDefaultKind(field);
            const bool relocatable =
                kind == EPathRefKind::Absolute || kind == EPathRefKind::PathUnderWorkingDir;
            // The synthetic working-dir entry has no field behind it; the
            // working-dir rewrite covers it.
            const bool exempt = field == EPathField::WorkingDirItself;
            // Absolute only when its value starts with a slash, so its table
            // default is LeafUnderSibling.
            const bool conditional = field == EPathField::AlterTable_Column_DefaultFromSequence;

            const bool has = CanRelocatePathField(field);
            setters += has ? 1 : 0;
            if ((relocatable && !exempt) || conditional) {
                if (!has) {
                    missing.push_back(name);
                }
            } else if (has) {
                unexpected.push_back(name);
            }
        }
        UNIT_ASSERT_C(missing.empty(), "no setter for " << JoinSeq(", ", missing));
        UNIT_ASSERT_C(unexpected.empty(), "setter for a field that never moves: "
            << JoinSeq(", ", unexpected));
        UNIT_ASSERT_C(setters > 30, "only " << setters << " setters");
    }

    Y_UNIT_TEST(StripApplyIf) {
        auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpDropTable, "/MyRoot/Dir");
        tx.MutableDrop()->SetName("T");
        for (ui64 pathId : {3ull, 4ull}) {
            auto* applyIf = tx.AddApplyIf();
            applyIf->SetPathId(pathId);
            applyIf->SetPathVersion(1);
        }

        const auto stripped = StripSourceLocalPreconditions(tx);

        UNIT_ASSERT_VALUES_EQUAL(stripped.size(), 2u);
        UNIT_ASSERT_EQUAL(stripped[0], EPathField::ApplyIf_PathId);
        UNIT_ASSERT_VALUES_EQUAL(tx.ApplyIfSize(), 0u);
        UNIT_ASSERT_VALUES_EQUAL(tx.GetDrop().GetName(), "T");
    }

    ////////////////////////////////////////////////////////////////////////////
    // Propose-level: the same rewrites driven by a footprint the schemeshard
    // itself resolved, checked against what the schemeshard accepts.

    Y_UNIT_TEST(CanonicalizedDropByIdEqualsTheByNameRequest) {
        TFootprintCollector collector;
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().PathFootprintObserver(&collector));
        ui64 txId = 100;

        TestMkDir(runtime, ++txId, "/MyRoot", "Dir");
        env.TestWaitNotification(runtime, txId);
        TestCreateTable(runtime, ++txId, "/MyRoot/Dir", R"(
            Name: "T"
            Columns { Name: "key" Type: "Uint64" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        const ui64 localPathId =
            DescribePath(runtime, "/MyRoot/Dir/T").GetPathDescription().GetSelf().GetPathId();

        // The by-id request, exactly as a client would send it, run through the
        // real schemeshard so that the footprint is the real one.
        NKikimrSchemeOp::TModifyScheme byId;
        {
            THolder<TEvTx> request(DropTableRequest(++txId, localPathId));
            byId = request->Record.GetTransaction(0);
        }
        const size_t mark = collector.Requests.size();
        SendModify(runtime, txId, byId);
        env.TestWaitNotification(runtime, txId);

        // The footprint the schemeshard itself resolved for this request.
        UNIT_ASSERT_VALUES_EQUAL(collector.Requests.size() - mark, 1u);
        const TPathFootprint& footprint = collector.Requests[mark].Footprint;

        auto canonical = byId;
        const auto result = CanonicalizeToPaths(canonical, footprint);
        UNIT_ASSERT(result.Changed);
        UNIT_ASSERT(result.Untransformable.empty());

        // The by-name request the helper builds for the same table. If the two
        // protos agree, canonicalization matches Propose() semantics on real
        // schemeshard state.
        NKikimrSchemeOp::TModifyScheme byName;
        {
            THolder<TEvTx> request(DropTableRequest(txId + 1, "/MyRoot/Dir", "T"));
            byName = request->Record.GetTransaction(0);
        }
        UNIT_ASSERT_VALUES_EQUAL(canonical.DebugString(), byName.DebugString());
    }

    Y_UNIT_TEST(CanonicalizedAlterTableByPathIdIsAcceptedByName) {
        TFootprintCollector collector;
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().PathFootprintObserver(&collector));
        ui64 txId = 100;

        TestMkDir(runtime, ++txId, "/MyRoot", "Dir");
        env.TestWaitNotification(runtime, txId);
        TestCreateTable(runtime, ++txId, "/MyRoot/Dir", R"(
            Name: "T"
            Columns { Name: "key" Type: "Uint64" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        const auto self = DescribePath(runtime, "/MyRoot/Dir/T").GetPathDescription().GetSelf();

        NKikimrSchemeOp::TModifyScheme byId;
        byId.SetOperationType(NKikimrSchemeOp::ESchemeOpAlterTable);
        TPathId(TOwnerId(self.GetSchemeshardId()), TLocalPathId(self.GetPathId()))
            .ToProto(byId.MutableAlterTable()->MutablePathId());
        {
            auto* column = byId.MutableAlterTable()->AddColumns();
            column->SetName("added");
            column->SetType("Uint64");
        }

        const size_t mark = collector.Requests.size();
        SendModify(runtime, ++txId, byId);
        env.TestWaitNotification(runtime, txId);

        // The footprint the schemeshard itself resolved for this request.
        UNIT_ASSERT_VALUES_EQUAL(collector.Requests.size() - mark, 1u);
        const TPathFootprint& footprint = collector.Requests[mark].Footprint;

        auto canonical = byId;
        UNIT_ASSERT(CanonicalizeToPaths(canonical, footprint).Changed);
        UNIT_ASSERT_VALUES_EQUAL(canonical.GetWorkingDir(), "/MyRoot/Dir");
        UNIT_ASSERT_VALUES_EQUAL(canonical.GetAlterTable().GetName(), "T");
        UNIT_ASSERT(!canonical.GetAlterTable().HasPathId());

        // The by-name form the schemeshard accepts for the same alter, which is
        // the proof that the rewrite is semantics-preserving and not just
        // string-equal.
        NKikimrSchemeOp::TModifyScheme byName = canonical;
        byName.MutableAlterTable()->MutableColumns(0)->SetName("added2");
        SendModify(runtime, ++txId, byName);
        env.TestWaitNotification(runtime, txId);

        const auto described = DescribePath(runtime, "/MyRoot/Dir/T");
        TVector<TString> columns;
        for (const auto& column : described.GetPathDescription().GetTable().GetColumns()) {
            columns.push_back(column.GetName());
        }
        Sort(columns);
        UNIT_ASSERT_VALUES_EQUAL(JoinSeq(",", columns), "added,added2,key");
    }

    Y_UNIT_TEST(RelocateDrivenByASchemeShardResolvedFootprint) {
        TFootprintCollector collector;
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().PathFootprintObserver(&collector));
        ui64 txId = 100;

        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "Src"
            Columns { Name: "key" Type: "Uint64" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        NKikimrSchemeOp::TModifyScheme move;
        move.SetOperationType(NKikimrSchemeOp::ESchemeOpMoveTable);
        move.SetWorkingDir("/MyRoot");
        move.MutableMoveTable()->SetSrcPath("/MyRoot/Src");
        move.MutableMoveTable()->SetDstPath("/MyRoot/Dst");

        const size_t mark = collector.Requests.size();
        SendModify(runtime, ++txId, move);
        env.TestWaitNotification(runtime, txId);

        // The footprint the schemeshard resolved, not one the test invented.
        UNIT_ASSERT_VALUES_EQUAL(collector.Requests.size() - mark, 1u);
        const TPathFootprint& footprint = collector.Requests[mark].Footprint;
        UNIT_ASSERT_VALUES_EQUAL(footprint.WorkingDir, "/MyRoot");
        UNIT_ASSERT_VALUES_EQUAL(footprint.Entries.size(), 3u);

        auto relocated = move;
        const auto result = RelocatePaths(relocated, footprint, {"/MyRoot", "/MyRoot2/x/db2"});

        UNIT_ASSERT(result.Changed);
        UNIT_ASSERT(result.Skipped.empty());
        UNIT_ASSERT_VALUES_EQUAL(relocated.GetWorkingDir(), "/MyRoot2/x/db2");
        UNIT_ASSERT_VALUES_EQUAL(relocated.GetMoveTable().GetSrcPath(), "/MyRoot2/x/db2/Src");
        UNIT_ASSERT_VALUES_EQUAL(relocated.GetMoveTable().GetDstPath(), "/MyRoot2/x/db2/Dst");
    }
}
