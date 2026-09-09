#pragma once

#include <yql/essentials/ast/yql_ast.h>

#include <util/generic/maybe.h>
#include <util/generic/vector.h>

namespace NKikimr::NKqp {

struct TQueryAst {
    TQueryAst(std::shared_ptr<NYql::TAstParseResult> ast, const TMaybe<ui16>& sqlVersion, const TMaybe<bool>& deprecatedSQL,
        bool keepInCache, const TMaybe<TString>& commandTagName)
        : Ast(std::move(ast))
        , SqlVersion(sqlVersion)
        , DeprecatedSQL(deprecatedSQL)
        , KeepInCache(keepInCache)
        , CommandTagName(commandTagName) {}

    std::shared_ptr<NYql::TAstParseResult> Ast;
    TMaybe<ui16> SqlVersion;
    TMaybe<bool> DeprecatedSQL;
    bool KeepInCache;
    TMaybe<TString> CommandTagName;
};

struct TNativeOperationAstInfo {
    bool HasSqlKey = false;
    ui32 Writes = 0;
    ui32 NativeWrites = 0;
    bool HasReads = false;

    bool IsSingleNativeOperation() const {
        return Writes == 1 && NativeWrites == 1 && !HasReads;
    }
};

inline TNativeOperationAstInfo InspectNativeOperationAst(const NYql::TAstNode* root) {
    TNativeOperationAstInfo result;
    const auto unquote = [](const NYql::TAstNode* node) {
        if (node->IsListOfSize(2) && node->GetChild(0)->IsAtom()
            && node->GetChild(0)->GetContent() == "quote")
        {
            return node->GetChild(1);
        }
        return node;
    };
    TVector<const NYql::TAstNode*> pending;
    if (root) {
        pending.push_back(root);
    }
    while (!pending.empty()) {
        const auto* node = pending.back();
        pending.pop_back();
        if (!node->IsList()) {
            continue;
        }
        // Inspect translated operations, not SQL comments or string literals.
        if (node->GetChildrenCount() && node->GetChild(0)->IsAtom()) {
            const auto callable = node->GetChild(0)->GetContent();
            result.HasReads |= callable == "Read!";
            if (callable == "Write!") {
                ++result.Writes;
                bool native = false;
                const auto* settings = node->IsListOfSize(6) ? unquote(node->GetChild(5)) : nullptr;
                if (settings && settings->IsList()) {
                    for (const auto* setting : settings->GetChildren()) {
                        setting = unquote(setting);
                        if (!setting->IsListOfSize(2)) {
                            continue;
                        }
                        const auto* name = unquote(setting->GetChild(0));
                        const auto* value = unquote(setting->GetChild(1));
                        if (!name->IsAtom()) {
                            continue;
                        }
                        result.HasSqlKey |= name->GetContent() == "uid";
                        if (name->GetContent() == "mode" && value->IsAtom()) {
                            const auto mode = value->GetContent();
                            native |= mode == "backup" || mode == "backupIncremental" || mode == "restore";
                        }
                    }
                }
                result.NativeWrites += native;
            }
        }
        for (const auto* child : node->GetChildren()) {
            pending.push_back(child);
        }
    }
    return result;
}

inline bool HasSqlNativeOperationUid(const NYql::TAstNode* root) {
    return InspectNativeOperationAst(root).HasSqlKey;
}

} // namespace NKikimr::NKqp
