UNITTEST_FOR(ydb/core/tx/schemeshard)

FORK_SUBTESTS()

SPLIT_FACTOR(4)

IF (SANITIZER_TYPE == "thread" OR WITH_VALGRIND)
    SIZE(LARGE)
    TAG(ya:fat)
ELSE()
    SIZE(MEDIUM)
ENDIF()

PEERDIR(
    ydb/core/protos
    ydb/core/tx/schemeshard/ut_helpers
    ydb/core/tx/datashard/ut_common
    ydb/core/kqp/ut/common
    ydb/core/testlib/default
    yql/essentials/sql/pg_dummy
)

SRCS(
    ut_parts_permutation.cpp
)

YQL_LAST_ABI_VERSION()

END()
