PROGRAM(ss_tool)

SRCS(
    main.cpp
)

PEERDIR(
    library/cpp/getopt
    ydb/tools/ss_tool/lib
)

END()

RECURSE(
    lib
)

RECURSE_FOR_TESTS(
    ut
)
