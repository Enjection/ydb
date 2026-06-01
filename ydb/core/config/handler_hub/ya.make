LIBRARY()

SRCS(
    config_kind_handler.h
    config_handler_hub.h
    config_handler_hub.cpp
)

PEERDIR(
    ydb/core/cms/console
    ydb/core/protos
    ydb/library/actors/core
    ydb/library/services
)

END()
