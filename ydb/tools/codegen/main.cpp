#include <string>
#include <fstream>
#include <iostream>
#include <vector>
#include <unordered_map>
#include <set>
#include <charconv>
#include <thread>
#include <mutex>
#include <array>

#include <clang/Tooling/CompilationDatabase.h>
#include <clang/Tooling/JSONCompilationDatabase.h>
#include <clang/Tooling/ReplacementsYaml.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Tooling/Refactoring.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/raw_os_ostream.h>
#include <clang/AST/ASTContext.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Lex/Preprocessor.h>

#include <util/system/compiler.h>
#include <util/system/types.h>
#include <util/thread/pool.h>
#include <util/system/guard.h>

using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::tooling;
using namespace llvm;

class MyHandler : public clang::ast_matchers::MatchFinder::MatchCallback
{
public:
    MyHandler(std::unordered_map<ui64, std::set<std::string>>& entries_) : entries(entries_) {}

    // Method is invoked when new enum declaration found in the input file
    void run(const clang::ast_matchers::MatchFinder::MatchResult& result) override {
        if (const clang::CXXRecordDecl* decl = result.Nodes.getNodeAs<clang::CXXRecordDecl>("ev")) {
            std::cout << decl->getQualifiedNameAsString() << std::endl;
            decl->forallBases([&](auto* base){
                if (base->getName().str() == "TEventBase") {
                    const auto *templ = llvm::dyn_cast<ClassTemplateSpecializationDecl>(base);
                    if (templ) {
                        for (auto arg : templ->getTemplateArgs().asArray()) {
                            auto kind = arg.getKind();
                            std::cout << "    ";
                            if (kind == clang::TemplateArgument::ArgKind::Type) {
                                std::cout << arg.getAsType().getAsString();
                            } else if (kind == clang::TemplateArgument::ArgKind::Integral) {
                                SmallString<20> ss;
                                arg.getAsIntegral().toString(ss);
                                ui64 value = 0;
                                std::from_chars(ss.c_str(), ss.c_str() + ss.size(), value);
                                std::cout << value;
                                entries[value].insert(decl->getQualifiedNameAsString());
                            }
                            std::cout << std::endl;
                        }
                    }
                }
                return true;
            });
        }
    }

    //auto& GetFoundEnums() const {return m_foundEnums;}

    std::unordered_map<ui64, std::set<std::string>>& entries;
private:

    /*
    // Collection of the found enum declaration
    std::vector<EnumDescriptor> m_foundEnums;

    void ProcessEnum(const clang::EnumDecl* decl)
    {
        EnumDescriptor descriptor;
        // Get name of the found enum
        descriptor.enumName = decl->getName();
        // Get 'isScoped' flag
        descriptor.isScoped = decl->isScoped();

        // Get enum items
        for (auto itemDecl : decl->enumerators())
            descriptor.enumItems.push_back(itemDecl->getName());

        std::sort(descriptor.enumItems.begin(), descriptor.enumItems.end());
        m_foundEnums.push_back(std::move(descriptor));
    }
    */
};

int main(int argc, char** argv) {
    Y_UNUSED(argc);
    std::string err;
    auto compilations = JSONCompilationDatabase::loadFromFile(argv[1], err, JSONCommandLineSyntax::AutoDetect);
    if (!compilations) {
        std::cout << err << std::endl;
        return 1;
    } else {
        std::cout << "compile commands are ok" << std::endl;
    }

    // it was base compilations
    auto allFiles = compilations->getAllFiles();
    int threadCount = 128;

    int threadChunkSize = allFiles.size() / threadCount;

    std::vector<
        std::unordered_map<ui64, std::set<std::string>>> entries;
    entries.resize(threadCount);
    std::array<std::mutex, 128> mutexes;
    for (int i = 0; i < threadCount; ++i) {
        std::vector<std::string> threadChunk;
        for (int j = 0; j < threadChunkSize && (i * threadChunkSize + j) < (int)allFiles.size(); ++j) {
            threadChunk.push_back(allFiles.at(i * threadChunkSize + j));
        }

        std::thread thr([files = threadChunk, &argv, &localEntries = entries[i], &mtx = mutexes[i]]() mutable {
            mtx.lock();
            std::string err;
            auto compilations = JSONCompilationDatabase::loadFromFile(argv[1], err, JSONCommandLineSyntax::AutoDetect);

            ClangTool tool(*compilations, files);
            DeclarationMatcher myMatcher =
                    cxxRecordDecl(
                        isDerivedFrom(
                            hasName("TEventPB")
                        )
                    ).bind("ev");

            MyHandler handler(localEntries);
            MatchFinder finder;
            finder.addMatcher(myMatcher, &handler);

            auto result = tool.run(newFrontendActionFactory(&finder).get());
            std::cout << result << std::endl;
            mtx.unlock();
        });
        thr.detach();
    }

    for (auto& mtx : mutexes) {
        mtx.lock();
    }

    std::unordered_map<ui64, std::set<std::string>> finalEntries;

    for (auto& entry : entries) {
        for (auto& [k, v] : entry) {
            for (auto& val : v) {
                finalEntries[k].insert(val);
            }
        }
    }

    std::ofstream ofs(argv[2]);
    for (auto& [k, v] : finalEntries) {
        ofs << k << " : ";
        for (auto& str : v) {
            ofs << str << " ";
        }
        if (v.size() > 1) {
            ofs << "+++";
        } else {
            ofs << "---";
        }
        ofs << '\n';
    }

    return 0;
}
