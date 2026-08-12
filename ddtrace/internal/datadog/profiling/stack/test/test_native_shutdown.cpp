#include "../../dd_wrapper/test/test_utils.hpp"
#include "sampler.hpp"
#include <gtest/gtest.h>

#include <Python.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace {

std::mutex thread_start_mutex;
std::condition_variable thread_start_cv;
bool thread_started = false;
bool release_thread = false;
bool thread_start_completed = false;
bool exit_handler_reached = false;
bool stop_returned_early = false;

void
block_sampling_thread_start()
{
    std::unique_lock<std::mutex> lock(thread_start_mutex);
    thread_started = true;
    thread_start_cv.notify_all();
    thread_start_cv.wait(lock, []() { return release_thread; });
    thread_start_completed = true;
    thread_start_cv.notify_all();
}

void
release_sampling_thread()
{
    std::unique_lock<std::mutex> lock(thread_start_mutex);
    // Exceed the former three-second stop timeout so this also verifies that
    // native shutdown never advances to profiler cleanup while sampling is active.
    stop_returned_early =
      thread_start_cv.wait_for(lock, std::chrono::milliseconds(3500), []() { return exit_handler_reached; });
    release_thread = true;
    thread_start_cv.notify_all();
}

void
verify_sampler_stopped()
{
    std::unique_lock<std::mutex> lock(thread_start_mutex);
    exit_handler_reached = true;
    thread_start_cv.notify_all();
    thread_start_cv.wait(lock, []() { return thread_start_completed; });
    if (stop_returned_early) {
        std::_Exit(4);
    }
}

}

TEST(NativeShutdownTest, ExitWhileSamplerThreadIsStarting)
{
    // Exiting this test process directly lets CTest validate the process status
    // without introducing a sanitizer-sensitive death-test subprocess.
    Py_Initialize();

    // Register this before profiler state and the sampler so it runs after their
    // native exit handlers and verifies that shutdown waited for the pending thread.
    std::atexit(verify_sampler_stopped);

    configure("shutdown-test", "test", "1.0", "http://127.0.0.1:8126", "cpython", "test", "test", 64);

    auto& sampler = Datadog::Sampler::get();
    sampler.set_thread_start_hook_for_testing(block_sampling_thread_start);
    sampler.set_interval(0.001);
    if (!sampler.start()) {
        std::exit(2);
    }

    {
        std::unique_lock<std::mutex> lock(thread_start_mutex);
        if (!thread_start_cv.wait_for(lock, std::chrono::seconds(1), []() { return thread_started; })) {
            std::exit(3);
        }
    }

    // Release the sampler only after native shutdown has begun. Its exit handler
    // must wait even though the sampling thread has not entered its main loop.
    std::thread(release_sampling_thread).detach();
    std::exit(0);
}
