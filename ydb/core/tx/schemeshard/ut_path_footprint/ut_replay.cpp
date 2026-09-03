#include <ydb/core/tx/schemeshard/schemeshard_path_footprint.h>
#include <ydb/core/tx/schemeshard/ut_helpers/helpers.h>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>

#include <util/generic/algorithm.h>
#include <util/generic/deque.h>
#include <util/generic/hash_set.h>
#include <util/string/builder.h>
#include <util/string/join.h>

#include <algorithm>

using namespace NKikimr;
using namespace NKikimr::NSchemeShard;
using namespace NSchemeShardUT_Private;

// The replay experiment of plan §8.7 / thoughts-replay-completeness §5, as a
// deterministic test.
//
// Claim P (what the extractor gives) is "every path a request names is in the
// footprint". Claim S is "streaming the patched requests somewhere else
// reproduces the same state". S does not follow from P, and this suite is the
// honest measurement of the gap: a fixed sequence of scheme requests is applied
// to one database, every request is then canonicalized and relocated using the
// footprint the schemeshard itself resolved, replayed into a second database,
// and the two subtrees are described recursively and diffed with the physical
// (target-decided) fields masked.
//
// Both databases are real subdomains of one schemeshard. A plain MkDir would
// not do: relocation is defined against TPath::GetDomainPathString(), so for a
// directory the database is still /MyRoot and the rewrite is a no-op. Two
// TTestBasicRuntimes in one test body have no precedent in the tree, and two
// subdomains test exactly what relocation is for.
namespace {

////////////////////////////////////////////////////////////////////////////////
// The two databases. dbB is one directory deeper than dbA, so relocation is not
// a rename: every rewritten path gains a segment and a purely-suffix rewrite
// would be caught.

const TString DbA = "/MyRoot/dbA";
const TString DbB = "/MyRoot/dir/dbB";

////////////////////////////////////////////////////////////////////////////////
// Observation channel. Only request-level footprints matter here: those are the
// ones §8.7 says may be replayed. Part footprints describe operations the
// schemeshard derived for itself, and replaying those would double-apply.

class TRequestFootprintCollector: public IPathFootprintObserver {
public:
    struct TItem {
        TTxId TxId;
        TPathFootprint Footprint;
    };

    void OnRequestFootprint(TTxId txId, const TPathFootprint& footprint) override {
        Requests.push_back(TItem{txId, footprint});
    }

    void OnPartFootprint(TTxId, const TPathFootprint&) override {
    }

    // A TDeque never relocates, so a reference taken now stays valid.
    TDeque<TItem> Requests;
};

////////////////////////////////////////////////////////////////////////////////
// Sending a hand-built request and reading back the status the schemeshard
// answered with, instead of asserting one. The whole point of the experiment is
// to record "accepted on A, rejected on B" when it happens.

NKikimrScheme::EStatus SendAndWait(TTestActorRuntime& runtime, TTestEnv& env, ui64 txId,
        const NKikimrSchemeOp::TModifyScheme& tx)
{
    auto* ev = new TEvTx(txId, TTestTxConfig::SchemeShard);
    *ev->Record.AddTransaction() = tx;
    AsyncSend(runtime, TTestTxConfig::SchemeShard, ev);

    TAutoPtr<IEventHandle> handle;
    TEvSchemeShard::TEvModifySchemeTransactionResult* event = nullptr;
    do {
        event = runtime.GrabEdgeEvent<TEvSchemeShard::TEvModifySchemeTransactionResult>(handle);
        UNIT_ASSERT(event);
    } while (event->Record.GetTxId() < txId);
    UNIT_ASSERT_VALUES_EQUAL(event->Record.GetTxId(), txId);

    const auto status = event->Record.GetStatus();
    if (status == NKikimrScheme::StatusAccepted) {
        env.TestWaitNotification(runtime, txId);
    }
    return status;
}

NKikimrSchemeOp::TModifyScheme MakeTx(NKikimrSchemeOp::EOperationType type, const TString& workingDir) {
    NKikimrSchemeOp::TModifyScheme tx;
    tx.SetOperationType(type);
    tx.SetWorkingDir(workingDir);
    return tx;
}

TString SubDomainScheme(const TString& name) {
    return TStringBuilder() << R"(
        Name: ")" << name << R"("
        PlanResolution: 50
        Coordinators: 1
        Mediators: 1
        TimeCastBucketsPerMediator: 2
        StoragePools { Name: "pool-1" Kind: "pool-kind-1" }
    )";
}

TVector<TString> FieldNames(const TVector<EPathField>& fields) {
    TVector<TString> names;
    for (auto field : fields) {
        names.push_back(TString(PathFieldName(field)));
    }
    return names;
}

////////////////////////////////////////////////////////////////////////////////
// Masking.
//
// "Same state" can only mean "same logical tree". Everything the *target*
// decides rather than the request is masked: identity (path ids, tablet ids,
// shard indexes), bookkeeping (versions, create tx/step), and statistics. The
// keys are fully qualified protobuf field names; MaskedFieldsExist below fails
// on a typo or a renamed field, so this set cannot silently stop masking.

const THashSet<TString> MaskedFields = {
    // Identity of the described path itself.
    "NKikimrScheme.TEvDescribeSchemeResult.PathId",
    "NKikimrScheme.TEvDescribeSchemeResult.PathOwnerId",
    "NKikimrScheme.TEvDescribeSchemeResult.DEPRECATED_PathOwner",
    "NKikimrScheme.TEvDescribeSchemeResult.LastExistedPrefixPathId",

    // TDirEntry: identity, parentage, and the version counters. The two
    // databases legitimately run different transaction sequences, so create
    // tx/step and every version differ by construction.
    "NKikimrSchemeOp.TDirEntry.PathId",
    "NKikimrSchemeOp.TDirEntry.SchemeshardId",
    "NKikimrSchemeOp.TDirEntry.CreateTxId",
    "NKikimrSchemeOp.TDirEntry.CreateStep",
    "NKikimrSchemeOp.TDirEntry.ParentPathId",
    "NKikimrSchemeOp.TDirEntry.PathVersion",
    "NKikimrSchemeOp.TDirEntry.Version",
    // EffectiveACL is inherited from the domain, so it differs by
    // construction; ACL is masked with it because the test sets neither.
    "NKikimrSchemeOp.TDirEntry.ACL",
    "NKikimrSchemeOp.TDirEntry.EffectiveACL",

    // Physical layout and statistics of a table.
    "NKikimrSchemeOp.TPathDescription.TablePartitions",
    "NKikimrSchemeOp.TPathDescription.TableStats",
    "NKikimrSchemeOp.TPathDescription.TabletMetrics",
    "NKikimrSchemeOp.TPathDescription.TablePartitionStats",
    "NKikimrSchemeOp.TPathDescription.TablePartitionMetrics",
    "NKikimrSchemeOp.TPathDescription.AbandonedTenantsSchemeShards",
    "NKikimrSchemeOp.TPathDescription.BackupProgress",
    "NKikimrSchemeOp.TPathDescription.LastBackupResult",
    // Coordinator/mediator tablet ids, pool bindings, the domain key, and the
    // per-domain path and shard counters.
    "NKikimrSchemeOp.TPathDescription.DomainDescription",

    // TTableDescription: identity, schema version, and everything the target
    // decides about partitioning and channel bindings.
    "NKikimrSchemeOp.TTableDescription.Id_Deprecated",
    "NKikimrSchemeOp.TTableDescription.PathId",
    "NKikimrSchemeOp.TTableDescription.TableSchemaVersion",
    "NKikimrSchemeOp.TTableDescription.CoordinatedSchemaVersion",
    "NKikimrSchemeOp.TTableDescription.PartitionConfig",
    "NKikimrSchemeOp.TTableDescription.UniformPartitionsCount",
    "NKikimrSchemeOp.TTableDescription.SplitBoundary",
    "NKikimrSchemeOp.TTableDescription.PartitionRangeBegin",
    "NKikimrSchemeOp.TTableDescription.PartitionRangeEnd",

    // Identity inside the typed descriptions.
    "NKikimrSchemeOp.TIndexDescription.LocalPathId",
    "NKikimrSchemeOp.TIndexDescription.PathOwnerId",
    "NKikimrSchemeOp.TIndexDescription.SchemaVersion",
    "NKikimrSchemeOp.TIndexDescription.DataSize",
    "NKikimrSchemeOp.TCdcStreamDescription.PathId",
    "NKikimrSchemeOp.TCdcStreamDescription.SchemaVersion",
    "NKikimrSchemeOp.TSequenceDescription.PathId",

    // The PQ group behind a cdc stream: partition/tablet identity and the
    // balancer tablet, none of which the request names.
    "NKikimrSchemeOp.TPersQueueGroupDescription.PathId",
    "NKikimrSchemeOp.TPersQueueGroupDescription.Partitions",
    "NKikimrSchemeOp.TPersQueueGroupDescription.BalancerTabletID",
    "NKikimrSchemeOp.TPersQueueGroupDescription.AlterVersion",
    "NKikimrSchemeOp.TPersQueueGroupDescription.NextPartitionId",
    // The topic config carries the owning path ids, the database path, the
    // per-partition tablet map and the YDB database id of its subdomain.
    "NKikimrSchemeOp.TPersQueueGroupDescription.PQTabletConfig",
};

void CollectFieldNames(const google::protobuf::Descriptor* descriptor,
        THashSet<TString>& visited, THashSet<TString>& out)
{
    if (!descriptor || !visited.insert(TString(descriptor->full_name())).second) {
        return;
    }
    for (int i = 0; i < descriptor->field_count(); ++i) {
        const auto* field = descriptor->field(i);
        out.insert(TString(field->full_name()));
        if (field->type() == google::protobuf::FieldDescriptor::TYPE_MESSAGE
                || field->type() == google::protobuf::FieldDescriptor::TYPE_GROUP) {
            CollectFieldNames(field->message_type(), visited, out);
        }
    }
}

// Replace a database prefix in a path-valued string, so that the two trees can
// be compared by their shape rather than by their absolute location.
bool RewriteDatabasePrefix(TString& value, const TString& databasePath) {
    if (value == databasePath) {
        value = "<db>";
        return true;
    }
    if (value.StartsWith(databasePath + "/")) {
        value = "<db>" + value.substr(databasePath.size());
        return true;
    }
    return false;
}

void Normalize(google::protobuf::Message& message, const TString& databasePath) {
    const auto* descriptor = message.GetDescriptor();
    const auto* reflection = message.GetReflection();

    for (int i = 0; i < descriptor->field_count(); ++i) {
        const auto* field = descriptor->field(i);
        if (MaskedFields.contains(TString(field->full_name()))) {
            reflection->ClearField(&message, field);
            continue;
        }

        switch (field->cpp_type()) {
            case google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE:
                if (field->is_repeated()) {
                    const int count = reflection->FieldSize(message, field);
                    for (int j = 0; j < count; ++j) {
                        Normalize(*reflection->MutableRepeatedMessage(&message, field, j), databasePath);
                    }
                } else if (reflection->HasField(message, field)) {
                    Normalize(*reflection->MutableMessage(&message, field), databasePath);
                }
                break;

            case google::protobuf::FieldDescriptor::CPPTYPE_STRING:
                // Bytes fields are blobs, not paths; only text is rewritten.
                if (field->type() != google::protobuf::FieldDescriptor::TYPE_STRING) {
                    break;
                }
                if (field->is_repeated()) {
                    const int count = reflection->FieldSize(message, field);
                    for (int j = 0; j < count; ++j) {
                        TString value = reflection->GetRepeatedString(message, field, j);
                        if (RewriteDatabasePrefix(value, databasePath)) {
                            reflection->SetRepeatedString(&message, field, j, value);
                        }
                    }
                } else if (reflection->HasField(message, field)) {
                    TString value = reflection->GetString(message, field);
                    if (RewriteDatabasePrefix(value, databasePath)) {
                        reflection->SetString(&message, field, value);
                    }
                }
                break;

            default:
                break;
        }
    }
}

////////////////////////////////////////////////////////////////////////////////
// The recursive describe. There is no such helper in the tree: ls_checks
// iterates GetChildren() one level deep only.

struct TTreeNode {
    // Path with the database prefix replaced by "<db>", so the two trees have
    // the same keys.
    TString RelPath;
    NKikimrScheme::TEvDescribeSchemeResult Describe;
};

// Describe returns Children for a directory, but a table only sets
// ChildrenExist and lists its indexes and cdc streams inside the table
// description. Both are exactly the derived children a relocated request never
// names, so the walk has to reach them.
TVector<TString> ChildNames(const NKikimrScheme::TEvDescribeSchemeResult& describe) {
    const auto& description = describe.GetPathDescription();

    TVector<TString> names;
    for (const auto& child : description.GetChildren()) {
        names.push_back(child.GetName());
    }
    for (const auto& index : description.GetTable().GetTableIndexes()) {
        names.push_back(index.GetName());
    }
    for (const auto& stream : description.GetTable().GetCdcStreams()) {
        names.push_back(stream.GetName());
    }
    SortUnique(names);
    return names;
}

void WalkTree(TTestActorRuntime& runtime, const TString& databasePath, const TString& path,
        TVector<TTreeNode>& out)
{
    // Private paths (index impl tables, the PQ group behind a cdc stream) are
    // exactly where relocation could go wrong, so they must be visible.
    auto describe = DescribePath(runtime, path,
        TDescribeOptionsBuilder().SetShowPrivateTable(true));
    UNIT_ASSERT_VALUES_EQUAL_C(describe.GetStatus(), NKikimrScheme::StatusSuccess, path);

    // GetChildren() is in TPathElement insertion order, which two independently
    // built trees have no reason to share.
    auto* children = describe.MutablePathDescription()->MutableChildren();
    std::sort(children->begin(), children->end(),
        [](const NKikimrSchemeOp::TDirEntry& l, const NKikimrSchemeOp::TDirEntry& r) {
            return l.GetName() < r.GetName();
        });

    const TVector<TString> names = ChildNames(describe);

    TString relPath = path;
    RewriteDatabasePrefix(relPath, databasePath);
    out.push_back(TTreeNode{relPath, std::move(describe)});

    for (const auto& name : names) {
        WalkTree(runtime, databasePath, path + "/" + name, out);
    }
}

TVector<TTreeNode> DescribeTree(TTestActorRuntime& runtime, const TString& databasePath) {
    TVector<TTreeNode> tree;
    WalkTree(runtime, databasePath, databasePath, tree);

    for (size_t i = 0; i < tree.size(); ++i) {
        Normalize(tree[i].Describe, databasePath);
    }
    // The database's own name is not something the replay reproduces: the two
    // databases are deliberately named differently. Everything below the root
    // keeps its name.
    UNIT_ASSERT(!tree.empty());
    tree[0].Describe.MutablePathDescription()->MutableSelf()->ClearName();
    return tree;
}

TVector<TString> PathsOf(const TVector<TTreeNode>& tree) {
    TVector<TString> paths;
    for (const auto& node : tree) {
        paths.push_back(node.RelPath);
    }
    return paths;
}

////////////////////////////////////////////////////////////////////////////////
// The per-request record that is the actual output of the experiment.

struct TStepResult {
    TString Name;
    NKikimrScheme::EStatus StatusA = NKikimrScheme::StatusSuccess;
    NKikimrScheme::EStatus StatusB = NKikimrScheme::StatusSuccess;
    bool Changed = false;
    TVector<TString> Stripped;
    TVector<TString> Untransformable;
    TVector<TString> Skipped;
};

TString FormatReport(const TVector<TStepResult>& report) {
    TStringBuilder out;
    out << "\nPathFootprint replay: " << report.size() << " request(s)\n";
    out << "  request | status on A | status on B | relocation changed the proto"
           " | untransformable | skipped\n";
    for (const auto& step : report) {
        out << "  " << step.Name
            << " | " << NKikimrScheme::EStatus_Name(step.StatusA)
            << " | " << NKikimrScheme::EStatus_Name(step.StatusB)
            << " | " << (step.Changed ? "yes" : "no")
            << " | " << (step.Untransformable ? JoinSeq(",", step.Untransformable) : TString("-"))
            << " | " << (step.Skipped ? JoinSeq(",", step.Skipped) : TString("-"))
            << "\n";
    }
    return out;
}

}  // namespace

Y_UNIT_TEST_SUITE(TSchemeShardPathFootprintReplay) {

    // A masked field name that no longer exists would silently stop masking.
    Y_UNIT_TEST(MaskedFieldsExist) {
        THashSet<TString> visited;
        THashSet<TString> reachable;
        CollectFieldNames(NKikimrScheme::TEvDescribeSchemeResult::descriptor(), visited, reachable);

        TVector<TString> stale;
        for (const auto& name : MaskedFields) {
            if (!reachable.contains(name)) {
                stale.push_back(name);
            }
        }
        Sort(stale);
        UNIT_ASSERT_C(stale.empty(),
            "masked field(s) unreachable from TEvDescribeSchemeResult (renamed or misspelled): "
            << JoinSeq(", ", stale));
    }

    // The experiment. Every assertion at the end is a claim about S:
    //   1. a request accepted on A is accepted on B after canonicalize+relocate;
    //   2. the two databases hold the same logical tree.
    Y_UNIT_TEST(RelocatedRequestsReproduceTheDatabase) {
        TRequestFootprintCollector collector;
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions()
            .PathFootprintObserver(&collector)
            .EnableProtoSourceIdInfo(true));
        ui64 txId = 100;

        TestCreateSubDomain(runtime, ++txId, "/MyRoot", SubDomainScheme("dbA"));
        env.TestWaitNotification(runtime, txId);
        TestMkDir(runtime, ++txId, "/MyRoot", "dir");
        env.TestWaitNotification(runtime, txId);
        TestCreateSubDomain(runtime, ++txId, "/MyRoot/dir", SubDomainScheme("dbB"));
        env.TestWaitNotification(runtime, txId);

        TVector<TStepResult> report;

        // Apply one original request to dbA, then rewrite a copy of it with the
        // footprint the schemeshard resolved for that very request and apply the
        // copy to dbB.
        auto replay = [&](const TString& name, const NKikimrSchemeOp::TModifyScheme& original) {
            TStepResult step;
            step.Name = name;

            const size_t mark = collector.Requests.size();
            step.StatusA = SendAndWait(runtime, env, ++txId, original);
            UNIT_ASSERT_VALUES_EQUAL_C(collector.Requests.size() - mark, 1u,
                name << ": expected exactly one request footprint");
            // A copy: canonicalization patches the footprint so that relocation
            // reads the request as rewritten rather than as submitted.
            TPathFootprint footprint = collector.Requests[mark].Footprint;

            auto rewritten = original;
            const TString before = rewritten.DebugString();

            step.Stripped = FieldNames(StripSourceLocalPreconditions(rewritten));
            step.Untransformable = FieldNames(CanonicalizeToPaths(rewritten, footprint).Untransformable);
            step.Skipped = FieldNames(RelocatePaths(rewritten, footprint,
                TRelocation{DbA, DbB}).Skipped);

            step.Changed = rewritten.DebugString() != before;

            step.StatusB = SendAndWait(runtime, env, ++txId, rewritten);
            report.push_back(step);
        };

        // 1. A leaf directly under the working dir.
        {
            auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpMkDir, DbA);
            tx.MutableMkDir()->SetName("dir");
            replay("MkDir dir", tx);
        }

        // 2. A multi-segment MkDir. SplitIntoTransactions turns it into several
        //    parts; the request footprint, not the derived parts, is what gets
        //    rewritten.
        {
            auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpMkDir, DbA);
            tx.MutableMkDir()->SetName("a/b");
            replay("MkDir a/b", tx);
        }

        // 3. One table, one shard, no data.
        {
            auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpCreateTable, DbA + "/dir");
            auto& table = *tx.MutableCreateTable();
            table.SetName("t");
            auto* key = table.AddColumns();
            key->SetName("key");
            key->SetType("Uint64");
            auto* value = table.AddColumns();
            value->SetName("value");
            value->SetType("Utf8");
            table.AddKeyColumnNames("key");
            replay("CreateTable t", tx);
        }

        // 4. Alter by name: one added column.
        {
            auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpAlterTable, DbA + "/dir");
            auto& alter = *tx.MutableAlterTable();
            alter.SetName("t");
            auto* column = alter.AddColumns();
            column->SetName("extra");
            column->SetType("Uint64");
            replay("AlterTable t add column", tx);
        }

        // 5. The compound case: the index and its impl table are never named in
        //    the request, so nothing about them can be relocated directly.
        {
            auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpCreateIndexedTable, DbA + "/dir");
            auto& indexed = *tx.MutableCreateIndexedTable();
            auto& table = *indexed.MutableTableDescription();
            table.SetName("it");
            auto* key = table.AddColumns();
            key->SetName("key");
            key->SetType("Uint64");
            auto* value = table.AddColumns();
            value->SetName("value");
            value->SetType("Utf8");
            table.AddKeyColumnNames("key");
            auto* index = indexed.AddIndexDescription();
            index->SetName("by_value");
            index->SetType(NKikimrSchemeOp::EIndexTypeGlobal);
            index->AddKeyColumnNames("value");
            replay("CreateIndexedTable it", tx);
        }

        // 6. A cdc stream, whose PQ group is derived under the stream.
        {
            auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpCreateCdcStream, DbA + "/dir");
            auto& cdc = *tx.MutableCreateCdcStream();
            cdc.SetTableName("t");
            auto& stream = *cdc.MutableStreamDescription();
            stream.SetName("Stream");
            stream.SetMode(NKikimrSchemeOp::ECdcStreamModeKeysOnly);
            stream.SetFormat(NKikimrSchemeOp::ECdcStreamFormatProto);
            replay("CreateCdcStream Stream", tx);
        }

        // 7. An absolute source path in a create request.
        {
            auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpCreateTable, DbA + "/dir");
            auto& table = *tx.MutableCreateTable();
            table.SetName("copy");
            table.SetCopyFromTable(DbA + "/dir/t");
            replay("CreateTable copy CopyFromTable", tx);
        }

        // 8. Two absolute paths in one request, both rewritten.
        {
            auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpMoveTable, DbA);
            tx.MutableMoveTable()->SetSrcPath(DbA + "/dir/copy");
            tx.MutableMoveTable()->SetDstPath(DbA + "/dir/moved");
            replay("MoveTable copy -> moved", tx);
        }

        // 9. Drop by name.
        {
            auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpDropTable, DbA + "/dir");
            tx.MutableDrop()->SetName("moved");
            replay("DropTable moved by name", tx);
        }

        // 10. A table that exists only to be dropped by id in step 11, so the
        //     indexed table and the cdc stream survive into the trees compared
        //     at the end.
        {
            auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpCreateTable, DbA + "/dir");
            auto& table = *tx.MutableCreateTable();
            table.SetName("tmp");
            auto* key = table.AddColumns();
            key->SetName("key");
            key->SetType("Uint64");
            table.AddKeyColumnNames("key");
            replay("CreateTable tmp", tx);
        }

        // 11. Drop by id. A path id means nothing in another database, so this
        //     is the case that forces CanonicalizeToPaths to run first, on the
        //     schemeshard that owns the id.
        //
        //     No working dir at all, which is how a by-id drop normally
        //     arrives: Propose() ignores it. Canonicalization invents one from
        //     the resolved path and patches the footprint, so relocation moves
        //     it into dbB. See CanonicalizeInventsTheWorkingDirRelocationMoves.
        {
            const auto self = DescribePath(runtime, DbA + "/dir/tmp")
                .GetPathDescription().GetSelf();
            auto tx = MakeTx(NKikimrSchemeOp::ESchemeOpDropTable, "");
            tx.MutableDrop()->SetId(self.GetPathId());
            replay("DropTable tmp by id", tx);
        }

        Cerr << FormatReport(report) << Endl;

        // Claim 1: relocation preserves acceptance.
        for (const auto& step : report) {
            UNIT_ASSERT_VALUES_EQUAL_C(NKikimrScheme::EStatus_Name(step.StatusA),
                NKikimrScheme::EStatus_Name(NKikimrScheme::StatusAccepted),
                step.Name << ": rejected on the source database");
            UNIT_ASSERT_VALUES_EQUAL_C(NKikimrScheme::EStatus_Name(step.StatusB),
                NKikimrScheme::EStatus_Name(step.StatusA),
                step.Name << ": the relocated copy was not accepted");
            UNIT_ASSERT_C(step.Untransformable.empty(),
                step.Name << ": untransformable " << JoinSeq(",", step.Untransformable));
        }

        // Nothing is skipped, the by-id drop included: canonicalization
        // rewrote its footprint entry into the name form before relocation
        // walked it.
        for (const auto& step : report) {
            UNIT_ASSERT_C(step.Skipped.empty(),
                step.Name << ": skipped " << JoinSeq(",", step.Skipped));
        }

        // Every request names at least a working dir under dbA, so relocation
        // has something to rewrite in each of them.
        for (const auto& step : report) {
            UNIT_ASSERT_C(step.Changed, step.Name << ": the rewrite was a no-op");
        }

        // Claim 2: the same logical tree.
        const auto treeA = DescribeTree(runtime, DbA);
        const auto treeB = DescribeTree(runtime, DbB);

        const TVector<TString> pathsA = PathsOf(treeA);
        Cerr << "PathFootprint replay: describe tree\n  " << JoinSeq("\n  ", pathsA) << Endl;

        UNIT_ASSERT_VALUES_EQUAL(JoinSeq("\n", pathsA), JoinSeq("\n", PathsOf(treeB)));
        for (size_t i = 0; i < treeA.size(); ++i) {
            UNIT_ASSERT_VALUES_EQUAL_C(treeA[i].Describe.DebugString(),
                treeB[i].Describe.DebugString(),
                "masked describe differs at " << treeA[i].RelPath);
        }

        // The derived paths no request ever names are the point of the diff, so
        // fail loudly if the walk stopped short of them.
        UNIT_ASSERT_VALUES_EQUAL(JoinSeq("\n", pathsA), JoinSeq("\n", TVector<TString>{
            "<db>",
            "<db>/a",
            "<db>/a/b",
            "<db>/dir",
            "<db>/dir/it",
            "<db>/dir/it/by_value",
            "<db>/dir/it/by_value/indexImplTable",
            "<db>/dir/t",
            "<db>/dir/t/Stream",
            "<db>/dir/t/Stream/streamImpl",
        }));
    }

    // The composition rule the experiment above depends on, pinned on its own
    // because getting it wrong silently replays into the source database.
    //
    // CanonicalizeToPaths writes a working dir derived from the resolved path
    // and patches the footprint with it, so RelocatePaths -- which rewrites
    // TPathFootprint::WorkingDirCanon -- sees the request as canonicalized
    // rather than as submitted. Both probes below must land in dbB: the one
    // whose request carried a working dir, and the one whose working dir only
    // exists because canonicalization invented it.
    Y_UNIT_TEST(CanonicalizeInventsTheWorkingDirRelocationMoves) {
        TRequestFootprintCollector collector;
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().PathFootprintObserver(&collector));
        ui64 txId = 100;

        TestCreateSubDomain(runtime, ++txId, "/MyRoot", SubDomainScheme("dbA"));
        env.TestWaitNotification(runtime, txId);
        TestMkDir(runtime, ++txId, DbA, "dir");
        env.TestWaitNotification(runtime, txId);
        for (TStringBuf name : {"t1", "t2"}) {
            TestCreateTable(runtime, ++txId, DbA + "/dir", TStringBuilder() << R"(
                Name: ")" << name << R"("
                Columns { Name: "key" Type: "Uint64" }
                KeyColumnNames: ["key"]
            )");
            env.TestWaitNotification(runtime, txId);
        }

        // Each probe drops its own table, so both run against live state and
        // the only difference between them is the working dir.
        const auto rewrite = [&](const TString& table, const TString& workingDir) {
            const auto self = DescribePath(runtime, DbA + "/dir/" + table)
                .GetPathDescription().GetSelf();

            NKikimrSchemeOp::TModifyScheme tx;
            tx.SetOperationType(NKikimrSchemeOp::ESchemeOpDropTable);
            tx.SetWorkingDir(workingDir);
            tx.MutableDrop()->SetId(self.GetPathId());

            // Proposing the request is how its footprint gets resolved by the
            // schemeshard that owns the id.
            const size_t mark = collector.Requests.size();
            UNIT_ASSERT_VALUES_EQUAL(
                NKikimrScheme::EStatus_Name(SendAndWait(runtime, env, ++txId, tx)),
                NKikimrScheme::EStatus_Name(NKikimrScheme::StatusAccepted));
            UNIT_ASSERT_VALUES_EQUAL(collector.Requests.size() - mark, 1u);
            TPathFootprint footprint = collector.Requests[mark].Footprint;

            auto rewritten = tx;
            CanonicalizeToPaths(rewritten, footprint);
            RelocatePaths(rewritten, footprint, TRelocation{DbA, DbB});
            return rewritten;
        };

        // No working dir at all: the only one the request ends up with is the
        // one canonicalization derived from the resolved path id.
        {
            const auto rewritten = rewrite("t1", "");
            UNIT_ASSERT_VALUES_EQUAL(rewritten.GetDrop().GetName(), "t1");
            UNIT_ASSERT_VALUES_EQUAL(rewritten.GetWorkingDir(), DbB + "/dir");
        }

        // A working dir the client sent, which canonicalization overwrites with
        // the same path. Relocation moves it either way.
        {
            const auto rewritten = rewrite("t2", DbA + "/dir");
            UNIT_ASSERT_VALUES_EQUAL(rewritten.GetDrop().GetName(), "t2");
            UNIT_ASSERT_VALUES_EQUAL(rewritten.GetWorkingDir(), DbB + "/dir");
        }
    }
}
