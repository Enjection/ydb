# Generated by devtools/yamaker.

LIBRARY()

LICENSE(Apache-2.0 WITH LLVM-exception)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

PEERDIR(
    contrib/libs/clang14
    contrib/libs/clang14/include
    contrib/libs/clang14/lib/ARCMigrate
    contrib/libs/clang14/lib/Basic
    contrib/libs/clang14/lib/CodeGen
    contrib/libs/clang14/lib/Driver
    contrib/libs/clang14/lib/Frontend
    contrib/libs/clang14/lib/Frontend/Rewrite
    contrib/libs/clang14/lib/StaticAnalyzer/Frontend
    contrib/libs/llvm14
    contrib/libs/llvm14/lib/Option
    contrib/libs/llvm14/lib/Support
)

ADDINCL(
    contrib/libs/clang14/lib/FrontendTool
)

NO_COMPILER_WARNINGS()

NO_UTIL()

SRCS(
    ExecuteCompilerInvocation.cpp
)

END()