UNITTEST()

SRCS(
    main.cpp
)

SIZE(MEDIUM)

PEERDIR(
    ydb/apps/version
    ydb/library/actors/interconnect/mock
    library/cpp/testing/unittest
    ydb/core/blobstorage/backpressure
    ydb/core/blobstorage/base
    ydb/core/blobstorage/dsproxy
    ydb/core/blobstorage/groupinfo
    ydb/core/blobstorage/pdisk/mock
    ydb/core/blobstorage/vdisk
    ydb/core/blobstorage/vdisk/common
    ydb/core/blobstorage/vdisk/repl
    ydb/core/tx/scheme_board
    ydb/core/util
    ydb/core/util/actorsys_test
)

END()
