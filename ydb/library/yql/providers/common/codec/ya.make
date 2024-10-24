LIBRARY()

SRCS(
    yql_codec.cpp
    yql_codec.h
    yql_codec_buf.cpp
    yql_codec_buf.h
    yql_codec_type_flags.cpp
    yql_codec_type_flags.h
    yql_json_codec.cpp
)

PEERDIR(
    ydb/library/yql/minikql/computation
    ydb/library/yql/parser/pg_wrapper/interface
    ydb/library/yql/providers/common/mkql
    ydb/library/yql/public/result_format
    library/cpp/yson/node
    library/cpp/yson
    library/cpp/json
    library/cpp/enumbitset
    yt/yt/library/decimal
)

YQL_LAST_ABI_VERSION()

GENERATE_ENUM_SERIALIZATION(yql_codec_type_flags.h)

END()

RECURSE(
    arrow
)

RECURSE_FOR_TESTS(
    ut
)
