LIBRARY()

LICENSE(MIT)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

NO_COMPILER_WARNINGS()

NO_RUNTIME()

CFLAGS(
    -DHAVE_CONFIG_H
    -DHASHER_SUFFIX=portable
    -DSIMD_DEGREE=1
)

ADDINCL(
    contrib/libs/libfyaml
    contrib/libs/libfyaml/include
    contrib/libs/libfyaml/src/util
    contrib/libs/libfyaml/src/thread
    contrib/libs/libfyaml/src/blake3
)

SRCS(
    ../src/blake3/blake3_portable.c
    ../src/blake3/blake3.c
)

END()
