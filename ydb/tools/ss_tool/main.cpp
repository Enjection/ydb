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

// One row in the tool's tables: identity (name + enum number) plus the
// trait-derived metadata. Identity is purely runtime info that does not
// belong on the trait; the rest is mirrored from TOpDescriptor in
// schemeshard__op_traits.h, which is the single source of truth for trait
// fields. Extending the trait does NOT require changing this file unless we
// want to expose the new field in `ops list` formatting.
struct TOpRow {
    TString Name;
    NSO::EOperationType Type{};
    TOpDescriptor Desc;
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

TOpRow CollectRow(NSO::EOperationType opType) {
    TOpRow row;
    row.Type = opType;
    row.Name = NSO::EOperationType_Name(opType);

    // DispatchOp routes by tx.GetOperationType(); feed it a synthetic tx and
    // hand the matched trait type to NSchemeShard::Describe<>().
    NSO::TModifyScheme tx;
    tx.SetOperationType(opType);
    DispatchOp(tx, [&](auto traits) {
        row.Desc = Describe<decltype(traits)>();
    });

    return row;
}

TVector<TOpRow> AllOps() {
    TVector<TOpRow> result;
    THashSet<int> seen;
    const auto* d = NSO::EOperationType_descriptor();
    for (int i = 0; i < d->value_count(); ++i) {
        const auto* v = d->value(i);
        if (!seen.insert(v->number()).second) {
            continue; // proto enum may carry aliases sharing one number
        }
        result.push_back(CollectRow(static_cast<NSO::EOperationType>(v->number())));
    }
    return result;
}

TString FormatFlags(const TOpDescriptor& d) {
    TStringBuilder sb;
    auto add = [&](const char* tag) {
        if (sb.size() > 0) {
            sb << ",";
        }
        sb << tag;
    };
    if (d.CreateDirsFromName)   add("CreateDirsFromName");
    if (d.CreateAdditionalDirs) add("CreateAdditionalDirs");
    if (d.NeedRewrite)          add("NeedRewrite");
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

void PrintRow(const TOpRow& op) {
    Cout << op.Name
         << "\t" << ClassName(op.Desc.Class)
         << "\t" << (op.Desc.HasMakeOperationParts   ? "yes" : "no")
         << "\t" << (op.Desc.HasCollectChangingPaths ? "yes" : "no")
         << "\t" << FormatFlags(op.Desc)
         << Endl;
}

bool MethodKnown(const TString& name) {
    return name == "MakeOperationParts" || name == "CollectChangingPaths";
}

bool HasMethod(const TOpDescriptor& d, const TString& name) {
    if (name == "MakeOperationParts")   return d.HasMakeOperationParts;
    if (name == "CollectChangingPaths") return d.HasCollectChangingPaths;
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
        if (!classFilter.empty() && TStringBuf(ClassName(op.Desc.Class)) != classFilter) {
            continue;
        }
        if (!hasFilter.empty() && !HasMethod(op.Desc, hasFilter)) {
            continue;
        }
        if (!missingFilter.empty() && HasMethod(op.Desc, missingFilter)) {
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
    const auto row = CollectRow(opType);
    const auto& d = row.Desc;
    Cout << "Name:                     " << row.Name << Endl;
    Cout << "Number:                   " << static_cast<int>(row.Type) << Endl;
    Cout << "Class:                    " << ClassName(d.Class) << Endl;
    Cout << "CreateDirsFromName:       " << (d.CreateDirsFromName       ? "true" : "false") << Endl;
    Cout << "CreateAdditionalDirs:     " << (d.CreateAdditionalDirs     ? "true" : "false") << Endl;
    Cout << "NeedRewrite:              " << (d.NeedRewrite              ? "true" : "false") << Endl;
    Cout << "HasMakeOperationParts:    " << (d.HasMakeOperationParts    ? "true" : "false") << Endl;
    Cout << "HasCollectChangingPaths:  " << (d.HasCollectChangingPaths  ? "true" : "false") << Endl;
    return 0;
}

int CmdMigrationStatus(int /*argc*/, const char** /*argv*/) {
    auto ops = AllOps();
    size_t total = ops.size();
    size_t fullyMigrated = 0;
    size_t partial = 0;
    for (const auto& op : ops) {
        if (op.Desc.HasMakeOperationParts && op.Desc.HasCollectChangingPaths) {
            ++fullyMigrated;
        } else if (op.Desc.HasMakeOperationParts || op.Desc.HasCollectChangingPaths) {
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
    THashMap<EOperationClass, TVector<const TOpRow*>> byClass;
    for (const auto& op : ops) {
        if (op.Desc.HasMakeOperationParts || op.Desc.HasCollectChangingPaths) {
            continue;
        }
        byClass[op.Desc.Class].push_back(&op);
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
