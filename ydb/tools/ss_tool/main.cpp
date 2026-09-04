// Read-only inspector for the schemeshard operation trait system.

#include <ydb/tools/ss_tool/lib/op_inspect.h>

#include <library/cpp/getopt/last_getopt.h>

#include <util/generic/hash.h>
#include <util/generic/string.h>
#include <util/generic/vector.h>
#include <util/stream/output.h>
#include <util/string/builder.h>

namespace {

using namespace NKikimr::NSchemeShard;
using namespace NKikimr::NSchemeShard::NSsTool;
namespace NSO = NKikimrSchemeOp;

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
