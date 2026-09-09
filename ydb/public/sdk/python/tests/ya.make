PY3TEST()

TEST_SRCS(
    test_query_idempotency.py
)

PEERDIR(
    ydb/public/sdk/python
)

END()
