// ss_tool — read-only inspector for the schemeshard operation trait system.
//
// The schemeshard trait registry (TSchemeTxTraits<EOperationType>) carries
// per-op metadata at compile time. The per-op `.cpp` view ("show me everything
// about CreateTable") lives in the source. This tool exposes the orthogonal
// by-aspect view ("show me all Create-class ops", "show me ops that still
// lack CollectChangingPaths") by iterating the proto enum and reading the
// trait fields via the existing DispatchOp infrastructure.

#include <ydb/core/tx/schemeshard/schemeshard__dispatch_op.h>
#include <ydb/core/tx/schemeshard/schemeshard__op_traits.h>

#include <ydb/core/protos/flat_scheme_op.pb.h>
#include <ydb/core/protos/schemeshard/operations.pb.h>

#include <library/cpp/getopt/last_getopt.h>

#include <util/generic/hash.h>
#include <util/generic/hash_set.h>
#include <util/generic/string.h>
#include <util/generic/vector.h>
#include <util/stream/output.h>
#include <util/string/builder.h>

#include <utility>

namespace {

using namespace NKikimr::NSchemeShard;
namespace NSO = NKikimrSchemeOp;

// Snapshot of the metadata we read out of TSchemeTxTraits<op>. Extend this
// struct (and CollectInfo below) when new trait fields land.
struct TOpInfo {
    TString Name;
    NSO::EOperationType Type{};
    EOperationClass Class = EOperationClass::Unknown;
    bool CreateDirsFromName = false;
    bool CreateAdditionalDirs = false;
    bool NeedRewrite = false;
    bool HasMakeOperationParts = false;
    bool HasCollectChangingPaths = false;
};

TStringBuf ClassName(EOperationClass c) {
    switch (c) {
        case EOperationClass::Create:  return "Create";
        case EOperationClass::Alter:   return "Alter";
        case EOperationClass::Drop:    return "Drop";
        case EOperationClass::Other:   return "Other";
        case EOperationClass::Unknown: return "Unknown";
    }
    return "?";
}

TOpInfo CollectInfo(NSO::EOperationType opType) {
    TOpInfo info;
    info.Type = opType;
    info.Name = NSO::EOperationType_Name(opType);

    // DispatchOp routes by tx.GetOperationType(); we feed it a synthetic tx.
    NSO::TModifyScheme tx;
    tx.SetOperationType(opType);

    DispatchOp(tx, [&](auto traits) {
        using Traits = decltype(traits);
        info.Class = Traits::Class;
        info.CreateDirsFromName = Traits::CreateDirsFromName;
        info.CreateAdditionalDirs = Traits::CreateAdditionalDirs;
        info.NeedRewrite = Traits::NeedRewrite;
        info.HasMakeOperationParts = requires {
            Traits::MakeOperationParts(
                std::declval<const TOperation&>(),
                std::declval<const TTxTransaction&>(),
                std::declval<TOperationContext&>());
        };
        info.HasCollectChangingPaths = requires {
            Traits::CollectChangingPaths(
                std::declval<const TTxTransaction&>(),
                std::declval<TVector<TString>&>());
        };
    });

    return info;
}

TVector<TOpInfo> AllOps() {
    TVector<TOpInfo> result;
    THashSet<int> seen;
    const auto* d = NSO::EOperationType_descriptor();
    for (int i = 0; i < d->value_count(); ++i) {
        const auto* v = d->value(i);
        if (!seen.insert(v->number()).second) {
            continue; // proto enum may carry aliases sharing one number
        }
        result.push_back(CollectInfo(static_cast<NSO::EOperationType>(v->number())));
    }
    return result;
}

TString FormatFlags(const TOpInfo& op) {
    TStringBuilder sb;
    auto add = [&](const char* tag) {
        if (sb.size() > 0) {
            sb << ",";
        }
        sb << tag;
    };
    if (op.CreateDirsFromName)   add("CreateDirsFromName");
    if (op.CreateAdditionalDirs) add("CreateAdditionalDirs");
    if (op.NeedRewrite)          add("NeedRewrite");
    return sb;
}

void PrintHeader() {
    Cout << "NAME"
         << "\tCLASS"
         << "\tMakeOperationParts"
         << "\tCollectChangingPaths"
         << "\tFLAGS"
         << Endl;
}

void PrintRow(const TOpInfo& op) {
    Cout << op.Name
         << "\t" << ClassName(op.Class)
         << "\t" << (op.HasMakeOperationParts   ? "yes" : "no")
         << "\t" << (op.HasCollectChangingPaths ? "yes" : "no")
         << "\t" << FormatFlags(op)
         << Endl;
}

bool MethodKnown(const TString& name) {
    return name == "MakeOperationParts" || name == "CollectChangingPaths";
}

bool HasMethod(const TOpInfo& op, const TString& name) {
    if (name == "MakeOperationParts")   return op.HasMakeOperationParts;
    if (name == "CollectChangingPaths") return op.HasCollectChangingPaths;
    return false;
}

int CmdList(int argc, const char** argv) {
    TString classFilter;
    TString hasFilter;
    TString missingFilter;

    NLastGetopt::TOpts opts;
    opts.AddLongOption("class", "filter by class: Create|Alter|Drop|Other|Unknown")
        .StoreResult(&classFilter);
    opts.AddLongOption("has", "only ops whose trait defines METHOD")
        .StoreResult(&hasFilter);
    opts.AddLongOption("missing", "only ops whose trait does NOT define METHOD")
        .StoreResult(&missingFilter);
    opts.AddHelpOption();
    NLastGetopt::TOptsParseResult res(&opts, argc, argv);

    if (!hasFilter.empty() && !MethodKnown(hasFilter)) {
        Cerr << "Unknown method '" << hasFilter
             << "'; expected MakeOperationParts or CollectChangingPaths" << Endl;
        return 1;
    }
    if (!missingFilter.empty() && !MethodKnown(missingFilter)) {
        Cerr << "Unknown method '" << missingFilter
             << "'; expected MakeOperationParts or CollectChangingPaths" << Endl;
        return 1;
    }

    PrintHeader();
    for (const auto& op : AllOps()) {
        if (!classFilter.empty() && TStringBuf(ClassName(op.Class)) != classFilter) {
            continue;
        }
        if (!hasFilter.empty() && !HasMethod(op, hasFilter)) {
            continue;
        }
        if (!missingFilter.empty() && HasMethod(op, missingFilter)) {
            continue;
        }
        PrintRow(op);
    }
    return 0;
}

int CmdShow(int argc, const char** argv) {
    if (argc < 2) {
        Cerr << "Usage: ss_tool ops show <OpName>" << Endl;
        return 1;
    }
    NSO::EOperationType opType{};
    if (!NSO::EOperationType_Parse(argv[1], &opType)) {
        Cerr << "Unknown op: " << argv[1] << Endl;
        return 1;
    }
    auto info = CollectInfo(opType);
    Cout << "Name:                     " << info.Name << Endl;
    Cout << "Number:                   " << static_cast<int>(info.Type) << Endl;
    Cout << "Class:                    " << ClassName(info.Class) << Endl;
    Cout << "CreateDirsFromName:       " << (info.CreateDirsFromName   ? "true" : "false") << Endl;
    Cout << "CreateAdditionalDirs:     " << (info.CreateAdditionalDirs ? "true" : "false") << Endl;
    Cout << "NeedRewrite:              " << (info.NeedRewrite          ? "true" : "false") << Endl;
    Cout << "HasMakeOperationParts:    " << (info.HasMakeOperationParts   ? "true" : "false") << Endl;
    Cout << "HasCollectChangingPaths:  " << (info.HasCollectChangingPaths ? "true" : "false") << Endl;
    return 0;
}

int CmdMigrationStatus(int /*argc*/, const char** /*argv*/) {
    auto ops = AllOps();
    size_t total = ops.size();
    size_t fullyMigrated = 0;
    size_t partial = 0;
    for (const auto& op : ops) {
        if (op.HasMakeOperationParts && op.HasCollectChangingPaths) {
            ++fullyMigrated;
        } else if (op.HasMakeOperationParts || op.HasCollectChangingPaths) {
            ++partial;
        }
    }
    Cout << "Total ops:      " << total << Endl;
    Cout << "Fully migrated: " << fullyMigrated
         << "  (trait defines both MakeOperationParts and CollectChangingPaths)" << Endl;
    Cout << "Partial:        " << partial << Endl;
    Cout << "Pending:        " << (total - fullyMigrated - partial) << Endl;
    Cout << Endl;

    Cout << "Pending ops grouped by Class:" << Endl;
    THashMap<EOperationClass, TVector<const TOpInfo*>> byClass;
    for (const auto& op : ops) {
        if (op.HasMakeOperationParts || op.HasCollectChangingPaths) {
            continue;
        }
        byClass[op.Class].push_back(&op);
    }
    for (auto cls : {EOperationClass::Create, EOperationClass::Alter,
                     EOperationClass::Drop,   EOperationClass::Other,
                     EOperationClass::Unknown}) {
        const auto& list = byClass[cls];
        if (list.empty()) {
            continue;
        }
        Cout << "  " << ClassName(cls) << " (" << list.size() << "):" << Endl;
        for (const auto* op : list) {
            Cout << "    " << op->Name << Endl;
        }
    }
    return 0;
}

void PrintUsage(const char* argv0) {
    Cerr << "Usage: " << argv0 << " ops <subcommand> [args...]" << Endl;
    Cerr << "Subcommands:" << Endl;
    Cerr << "  list [--class=X] [--has=METHOD] [--missing=METHOD]" << Endl;
    Cerr << "  show <OpName>" << Endl;
    Cerr << "  migration-status" << Endl;
}

} // namespace

int main(int argc, const char** argv) {
    if (argc < 3 || TString(argv[1]) != "ops") {
        PrintUsage(argv[0]);
        return 1;
    }
    const TString sub = argv[2];
    if (sub == "list")             return CmdList(argc - 2, argv + 2);
    if (sub == "show")             return CmdShow(argc - 2, argv + 2);
    if (sub == "migration-status") return CmdMigrationStatus(argc - 2, argv + 2);
    PrintUsage(argv[0]);
    return 1;
}
