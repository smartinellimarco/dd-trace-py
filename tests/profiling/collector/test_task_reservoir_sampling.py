"""Tests for adaptive task-sampling.

Verifies that when the number of leaf asyncio tasks exceeds
_DD_PROFILING_STACK_MAX_TASKS, the profiler:
  1. Emits at most MAX_TASKS wall-time samples per sampling tick (bounded count).
  2. Scales non-slot-0 wall time so the per-thread wall-time total is preserved.
"""

import pytest


# Keep MAX_TASKS low enough to exercise the cap easily with N_TASKS tasks.
_MAX_TASKS = 5


@pytest.mark.subprocess(
    env=dict(
        DD_PROFILING_OUTPUT_PPROF="/tmp/test_task_reservoir_sampling",
        DD_PROFILING_UPLOAD_INTERVAL="1",
        # Set the cap well below the number of tasks we spawn.
        _DD_PROFILING_STACK_MAX_TASKS=str(_MAX_TASKS),
    ),
    err=None,
)
def test_task_reservoir_sampling_bounded_count() -> None:
    """Number of distinct task-name samples per sampling interval is at most MAX_TASKS."""
    import asyncio
    import os
    import time

    from ddtrace.internal.datadog.profiling import stack
    from ddtrace.profiling import profiler
    from ddtrace.trace import tracer
    from tests.profiling.collector import pprof_utils
    from tests.profiling.collector.test_utils import async_run

    assert stack.is_available, stack.failure_msg

    MAX_TASKS = int(os.environ["_DD_PROFILING_STACK_MAX_TASKS"])
    N_TASKS = 60

    async def sleeper(name: str) -> None:
        # Sleep long enough that all tasks are alive during profiling.
        await asyncio.sleep(3.0)

    async def main() -> None:
        tasks = [asyncio.create_task(sleeper(f"worker-{i}"), name=f"worker-{i}") for i in range(N_TASKS)]
        await asyncio.gather(*tasks)

    p = profiler.Profiler(tracer=tracer)
    p.start()

    async_run(main())

    time.sleep(0.5)
    p.stop()

    output_filename = os.environ["DD_PROFILING_OUTPUT_PPROF"] + "." + str(os.getpid())
    profile = pprof_utils.parse_newest_profile(output_filename)

    # Collect all task-name samples (wall-time only; filter on "task name" label).
    task_samples = pprof_utils.get_samples_with_label_key(profile, "task name")
    assert len(task_samples) > 0, "Expected at least one task-name sample"

    # Determine the event-loop thread ID from one of the task samples.
    thread_id_label = pprof_utils.get_label_with_key(profile.string_table, task_samples[0], "thread id")
    assert thread_id_label is not None, "Task sample missing 'thread id' label"
    el_thread_id = thread_id_label.num

    # Count thread-level samples (no task label) for the event-loop thread -- one per sampling tick.
    el_thread_samples = [
        s
        for s in profile.sample
        if not pprof_utils.get_label_with_key(profile.string_table, s, "task name")
        and (tid := pprof_utils.get_label_with_key(profile.string_table, s, "thread id")) is not None
        and tid.num == el_thread_id
    ]
    num_ticks = len(el_thread_samples)
    assert num_ticks > 0, "Expected at least one thread-level sample for the event-loop thread"

    # Each tick produces at most MAX_TASKS task samples. Allow +1 tick tolerance for startup/shutdown.
    assert len(task_samples) <= (num_ticks + 1) * MAX_TASKS, (
        f"Total task samples ({len(task_samples)}) exceeds bounded cap "
        f"({(num_ticks + 1) * MAX_TASKS} = ({num_ticks}+1 ticks) * {MAX_TASKS} max_tasks)"
    )


@pytest.mark.subprocess(
    env=dict(
        DD_PROFILING_OUTPUT_PPROF="/tmp/test_task_reservoir_walltime",
        DD_PROFILING_UPLOAD_INTERVAL="1",
        _DD_PROFILING_STACK_MAX_TASKS=str(_MAX_TASKS),
    ),
    err=None,
)
def test_task_reservoir_sampling_walltime_scaling() -> None:
    """Total wall time across task samples is approximately N * wall_time_per_tick."""
    import asyncio
    import os
    import time

    from ddtrace.internal.datadog.profiling import stack
    from ddtrace.profiling import profiler
    from ddtrace.trace import tracer
    from tests.profiling.collector import pprof_utils
    from tests.profiling.collector.test_utils import async_run

    assert stack.is_available, stack.failure_msg

    N_TASKS = 60
    SLEEP_DURATION = 3.0
    TOLERANCE = 0.1  # allow 10% deviation from ideal

    async def sleeper() -> None:
        await asyncio.sleep(SLEEP_DURATION)

    async def main() -> None:
        tasks = [asyncio.create_task(sleeper(), name=f"worker-{i}") for i in range(N_TASKS)]
        await asyncio.gather(*tasks)

    p = profiler.Profiler(tracer=tracer)
    p.start()

    async_run(main())

    time.sleep(0.5)
    p.stop()

    output_filename = os.environ["DD_PROFILING_OUTPUT_PPROF"] + "." + str(os.getpid())
    profile = pprof_utils.parse_newest_profile(output_filename)

    wall_time_idx = pprof_utils.get_sample_type_index(profile, "wall-time")

    # Sum wall time for all task-name samples.
    task_samples = pprof_utils.get_samples_with_label_key(profile, "task name")
    assert len(task_samples) > 0

    total_task_walltime_ns = sum(s.value[wall_time_idx] for s in task_samples)

    # Identify the event-loop thread from the task samples.
    thread_id_label = pprof_utils.get_label_with_key(profile.string_table, task_samples[0], "thread id")
    assert thread_id_label is not None, "Task sample missing 'thread id' label"
    el_thread_id = thread_id_label.num

    # Sum wall time for thread-level samples (no "task name" label) on the event-loop thread only.
    thread_samples = [
        s
        for s in profile.sample
        if not pprof_utils.get_label_with_key(profile.string_table, s, "task name")
        and (tid := pprof_utils.get_label_with_key(profile.string_table, s, "thread id")) is not None
        and tid.num == el_thread_id
    ]
    total_thread_walltime_ns = sum(s.value[wall_time_idx] for s in thread_samples)

    if total_thread_walltime_ns == 0:
        # Fall back: skip the ratio check if there are no plain thread samples.
        exit(0)

    # Ideally total_task_walltime = N_TASKS * total_thread_walltime (one sample per task per tick).
    # With reservoir sampling it should still equal that (due to scaling).
    # Allow a generous tolerance for startup/shutdown noise.
    expected_min = N_TASKS * total_thread_walltime_ns * (1.0 - TOLERANCE)
    expected_max = N_TASKS * total_thread_walltime_ns * (1.0 + TOLERANCE)
    assert total_task_walltime_ns >= expected_min, (
        f"Total task wall time {total_task_walltime_ns}ns is less than "
        f"{expected_min}ns ({N_TASKS} * thread wall time * {1 - TOLERANCE})"
    )
    assert total_task_walltime_ns <= expected_max, (
        f"Total task wall time {total_task_walltime_ns}ns exceeds "
        f"{expected_max}ns ({N_TASKS} * thread wall time * {1 + TOLERANCE})"
    )
