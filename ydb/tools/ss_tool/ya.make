PROGRAM(ss_tool)

SRCS(
    main.cpp
)

PEERDIR(
    library/cpp/getopt
    library/cpp/json
    ydb/tools/ss_tool/lib
)

END()

RECURSE(
    lib
)

RECURSE_FOR_TESTS(
    ut
)
