UNITTEST_FOR(ydb/core/tx/schemeshard)

SIZE(SMALL)

PEERDIR(
    ydb/core/protos
    ydb/core/tablet_flat
    ydb/library/actors/core
)

SRCS(
    op_handlers_ut.cpp
)

END()
