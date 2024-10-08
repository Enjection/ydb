# Generated by devtools/yamaker.

LIBRARY()

VERSION(16.0.0)

LICENSE(Apache-2.0 WITH LLVM-exception)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

PEERDIR(
    contrib/libs/llvm16
    contrib/libs/llvm16/include
    contrib/libs/llvm16/lib/MC
    contrib/libs/llvm16/lib/MC/MCParser
    contrib/libs/llvm16/lib/MCA
    contrib/libs/llvm16/lib/Support
    contrib/libs/llvm16/lib/Target/X86/MCTargetDesc
    contrib/libs/llvm16/lib/Target/X86/TargetInfo
    contrib/libs/llvm16/lib/TargetParser
)

ADDINCL(
    ${ARCADIA_BUILD_ROOT}/contrib/libs/llvm16/lib/Target/X86
    contrib/libs/llvm16/lib/Target/X86
    contrib/libs/llvm16/lib/Target/X86/MCA
)

NO_COMPILER_WARNINGS()

NO_UTIL()

SRCS(
    X86CustomBehaviour.cpp
)

END()
