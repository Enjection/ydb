import asyncio
import threading
from contextlib import asynccontextmanager, contextmanager
from types import SimpleNamespace

import pytest

from ydb import issues
from ydb.aio.query.pool import QuerySessionPool as AsyncPool
from ydb.aio.query.session import QuerySession as AsyncSession
from ydb.query.base import QueryClientSettings, QueryExecMode, create_execute_query_request
from ydb.query.pool import QuerySessionPool
from ydb.query.session import QuerySession
from ydb.retries import BackoffSettings, RetrySettings

DDL = "-- keep original bytes\nBACKUP `daily`;"
KEY = "ключ with spaces/and?symbols!"


@pytest.mark.parametrize("key", [None, "", KEY, "a" * 128, "я" * 64, "a\0b"])
def test_request_presence_and_bytes(key):
    request = create_execute_query_request(
        query=DDL,
        session_id="session",
        tx_id=None,
        commit_tx=True,
        tx_mode=None,
        syntax=None,
        exec_mode=None,
        stats_mode=None,
        schema_inclusion_mode=None,
        result_set_format=None,
        arrow_format_settings=None,
        parameters=None,
        concurrent_result_sets=None,
        pool_id=None,
        uid=key,
    ).to_proto()
    assert request.HasField("uid") == (key is not None)
    if key is not None:
        assert request.uid == key
    assert request.exec_mode == QueryExecMode.EXECUTE
    assert request.query_content.text == DDL
    assert not request.HasField("tx_control")


@pytest.mark.parametrize("mode", [QueryExecMode.PARSE, QueryExecMode.VALIDATE, QueryExecMode.EXPLAIN])
def test_non_executing_modes_are_preserved(mode):
    request = create_execute_query_request(
        query=DDL,
        session_id="session",
        tx_id=None,
        commit_tx=True,
        tx_mode=None,
        syntax=None,
        exec_mode=mode,
        stats_mode=None,
        schema_inclusion_mode=None,
        result_set_format=None,
        arrow_format_settings=None,
        parameters=None,
        concurrent_result_sets=None,
        pool_id=None,
        uid=KEY,
    ).to_proto()
    assert request.uid == KEY
    assert request.exec_mode == mode
    assert request.query_content.text == DDL


class CaptureDriver:
    def __init__(self, retry=False):
        self.requests = []
        self.retry = retry

    def __call__(self, request, *args, **kwargs):
        self.requests.append(request)
        if self.retry and len(self.requests) == 1:
            raise issues.Unavailable("lost response")
        return iter(())


class AsyncCaptureDriver(CaptureDriver):
    async def __call__(self, request, *args, **kwargs):
        super().__call__(request, *args, **kwargs)

        async def empty_stream():
            for response in ():
                yield response

        return empty_stream()


def make_session(driver, index=0, asynchronous=False):
    cls = AsyncSession if asynchronous else QuerySession
    session = cls(driver, QueryClientSettings())
    session._session_id = "session-" + str(index)
    return session


def check_captured(driver, count):
    assert len(driver.requests) == count
    for request in driver.requests:
        assert request.HasField("uid")
        assert request.uid == KEY
        assert request.exec_mode == QueryExecMode.EXECUTE
        assert request.query_content.text == DDL


def test_sync_session():
    driver = CaptureDriver()
    session = make_session(driver)
    assert list(session.execute(DDL, uid=KEY)) == []
    check_captured(driver, 1)


def test_async_session():
    async def run():
        driver = AsyncCaptureDriver()
        session = make_session(driver, asynchronous=True)
        stream = await session.execute(DDL, uid=KEY)
        assert [part async for part in stream] == []
        check_captured(driver, 1)

    asyncio.run(run())


def retry_settings():
    return RetrySettings(
        max_retries=1,
        idempotent=True,
        fast_backoff_settings=BackoffSettings(slot_duration=0),
        slow_backoff_settings=BackoffSettings(slot_duration=0),
    )


def test_sync_pool_retry_with_replacement_session():
    driver = CaptureDriver(retry=True)

    @contextmanager
    def checkout(**kwargs):
        yield make_session(driver, len(driver.requests))

    pool = SimpleNamespace(checkout=checkout, _should_stop=threading.Event())
    assert QuerySessionPool.execute_with_retries(pool, DDL, retry_settings=retry_settings(), uid=KEY) == []
    check_captured(driver, 2)
    assert driver.requests[0].session_id != driver.requests[1].session_id


def test_async_pool_retry_with_replacement_session():
    async def run():
        driver = AsyncCaptureDriver(retry=True)

        @asynccontextmanager
        async def checkout(**kwargs):
            yield make_session(driver, len(driver.requests), asynchronous=True)

        pool = SimpleNamespace(checkout=checkout)
        assert await AsyncPool.execute_with_retries(pool, DDL, retry_settings=retry_settings(), uid=KEY) == []
        check_captured(driver, 2)
        assert driver.requests[0].session_id != driver.requests[1].session_id

    asyncio.run(run())
