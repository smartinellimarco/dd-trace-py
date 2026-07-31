"""Shared utilities for profiling collector tests."""

import asyncio
import os
from types import TracebackType
from typing import Any
from typing import Coroutine
from typing import Optional
from typing import TypeVar

from ddtrace.internal.datadog.profiling import ddup
from ddtrace.profiling import profiler


T = TypeVar("T")


def init_ddup(test_name: str) -> None:
    """Initialize ddup for profiling tests.

    Must be called before using any lock collectors.

    Args:
        test_name: Name of the test, used for service name and output filename.
    """
    assert ddup.is_available, "ddup is not available"
    ddup.config(
        env="test",
        service=test_name,
        version="1.0",
        output_filename="/tmp/" + test_name,
    )
    ddup.start()


def async_run(coro: Coroutine[Any, Any, T]) -> T:
    use_uvloop = os.environ.get("USE_UVLOOP", "0") == "1"

    if use_uvloop:
        import uvloop

        return uvloop.run(coro)  # type: ignore[no-any-return]
    else:
        # asyncio.run on Python 3.12+ uses asyncio.Runner which does not call
        # set_event_loop, so the profiler's wrapper never fires and the loop
        # is never tracked. Explicitly create and set the loop so the profiler
        # can discover asyncio tasks.
        # In production this isn't a problem: the profiler starts inside an
        # already-running loop, so link_existing_loop_to_current_thread picks
        # it up via get_running_loop. Tests start the profiler before the loop.
        # We have tests specifically around showing/reproducing that behaviour and
        # it is a known limitation that manually starting the profiler before the
        # loop starts makes us blind to the event loop.
        loop = asyncio.new_event_loop()
        asyncio.set_event_loop(loop)
        try:
            return loop.run_until_complete(coro)
        finally:
            loop.close()


def uvloop_available() -> bool:
    try:
        import uvloop  # noqa: F401

        return True
    except ImportError:
        return False


class ProfilerContextManager:
    def __init__(self) -> None:
        self.profiler = profiler.Profiler()

    def __enter__(self) -> profiler.Profiler:
        self.profiler.start()
        return self.profiler

    def __exit__(
        self,
        exc_type: Optional[type[BaseException]],
        exc_value: Optional[BaseException],
        traceback: Optional[TracebackType],
    ) -> None:
        self.profiler.stop()
