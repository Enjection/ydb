UNITTEST_FOR(ydb/core/blobstorage/vdisk/anubis_osiris)

FORK_SUBTESTS()

SIZE(MEDIUM)

PEERDIR(
    library/cpp/getopt
    library/cpp/svnversion
    ydb/core/base
)

SRCS(
    blobstorage_anubis_algo_ut.cpp
)

END()
