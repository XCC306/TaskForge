#include "taskforge/thread_pool.hpp"
#include "taskforge/wav_analyzer.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace std::chrono_literals;
using taskforge::TaskStatus;

void require(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    std::size_t entered_count = 0;
    bool released = false;
    void enter_and_wait() {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        ++entered_count;
        changed.notify_all();
        changed.wait(lock, [this] { return released; });
    }
    bool wait_entered() {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, 5s, [this] { return entered; });
    }
    bool wait_entered_count(std::size_t count) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, 5s, [this, count] { return entered_count == count; });
    }
    void release() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            released = true;
        }
        changed.notify_all();
    }
};
struct ReleaseOnExit {
    std::shared_ptr<Gate> gate;
    ~ReleaseOnExit() { gate->release(); }
};

template <class Result>
bool get_cancelled(taskforge::TaskHandle<Result>& handle) {
    try { static_cast<void>(handle.get()); }
    catch (const taskforge::TaskCancelled&) { return true; }
    return false;
}

void queued_cancel_is_immediate_and_skips_callable() {
    auto gate = std::make_shared<Gate>();
    std::atomic<unsigned> called{0};
    taskforge::ThreadPool pool(1);
    ReleaseOnExit release{gate};
    auto running = pool.submit_task([gate] { gate->enter_and_wait(); return 5; });
    require(gate->wait_entered(), "worker did not start");
    auto queued = pool.submit_task([&called] { ++called; return 7; });
    require(queued.status() == TaskStatus::Queued, "pending state");
    require(queued.cancel() && !queued.cancel(), "cancel must be idempotent");
    require(queued.wait_for(0ms) == std::future_status::ready,
            "pending cancellation must not wait for a free worker");
    require(get_cancelled(queued), "pending future must throw TaskCancelled");
    require(queued.status() == TaskStatus::Cancelled, "pending terminal state");
    gate->release();
    require(running.get() == 5, "other task changed by cancellation");
    pool.wait();
    require(called.load() == 0, "cancelled pending callable executed");
}

void running_cancel_waits_for_callable_exit() {
    auto gate = std::make_shared<Gate>();
    std::atomic<bool> observed{false};
    taskforge::ThreadPool pool(1);
    ReleaseOnExit release{gate};
    auto task = pool.submit_cancellable([gate, &observed](taskforge::CancellationToken token) {
        gate->enter_and_wait();
        observed.store(token.stop_requested());
        token.throw_if_cancelled();
        return 9;
    });
    require(gate->wait_entered(), "cooperative task did not start");
    require(task.status() == TaskStatus::Running, "running state");
    require(task.cancel(), "running cancellation rejected");
    require(task.cancellation_requested(), "stop token flag missing");
    require(task.status() == TaskStatus::Running, "cancel request pretended task had stopped");
    require(task.wait_for(0ms) == std::future_status::timeout,
            "running future ready before callable exit");
    gate->release();
    require(get_cancelled(task), "cooperative future did not report cancellation");
    require(observed.load() && task.status() == TaskStatus::Cancelled,
            "cooperative stop was not observed");
    require(!task.cancel(), "terminal cancel changed task");
}

void noncooperative_cancel_does_not_kill_or_undo_work() {
    auto gate = std::make_shared<Gate>();
    std::atomic<unsigned> side_effects{0};
    taskforge::ThreadPool pool(1);
    ReleaseOnExit release{gate};
    auto task = pool.submit_task([gate, &side_effects] {
        gate->enter_and_wait();
        ++side_effects;
        return 3;
    });
    require(gate->wait_entered(), "plain task did not start");
    require(task.cancel(), "plain running cancellation request");
    require(task.status() == TaskStatus::Running, "plain task falsely stopped");
    gate->release();
    require(get_cancelled(task), "cancel request must discard late value");
    require(side_effects.load() == 1, "cancellation must not promise side-effect rollback");
}

void timeout_is_only_a_wait_result() {
    auto gate = std::make_shared<Gate>();
    taskforge::ThreadPool pool(1);
    ReleaseOnExit release{gate};
    auto task = pool.submit_task([gate] { gate->enter_and_wait(); return 42; });
    require(gate->wait_entered(), "timeout task did not start");
    require(task.wait_for(10ms) == std::future_status::timeout, "wait did not time out");
    require(task.status() == TaskStatus::Running && !task.cancellation_requested(),
            "wait timeout mutated task");
    gate->release();
    require(task.get() == 42, "cannot receive result after wait timeout");
    require(task.status() == TaskStatus::Succeeded && !task.cancel(), "success terminal state");
    bool consumed = false;
    try { static_cast<void>(task.get()); }
    catch (const std::future_error&) { consumed = true; }
    require(consumed && !task.valid(), "get must consume future once");
}

void results_failures_and_moved_handles() {
    int number = 8;
    taskforge::ThreadPool pool(2);
    auto value = pool.submit_task(
        [owner = std::make_unique<int>(11)](std::unique_ptr<int> other) {
            return std::make_unique<int>(*owner + *other);
        }, std::make_unique<int>(4));
    auto moved = std::move(value);
    bool no_state = false;
    try { static_cast<void>(value.status()); }
    catch (const std::future_error&) { no_state = true; }
    require(no_state && !value.valid(), "moved-from handle should have no state");
    require(*moved.get() == 15 && moved.status() == TaskStatus::Succeeded,
            "move-only handle or result");
    auto reference = pool.submit_task([](int& n) -> int& { return n; }, std::ref(number));
    require(&reference.get() == &number, "reference return lost");
    auto nothing = pool.submit_cancellable(
        [](taskforge::CancellationToken token, int& n) {
            token.throw_if_cancelled();
            ++n;
        }, std::ref(number));
    nothing.get();
    require(number == 9 && nothing.status() == TaskStatus::Succeeded, "void/token/argument");
    auto failed = pool.submit_task([]() -> int { throw std::runtime_error("expected"); });
    bool caught = false;
    try { static_cast<void>(failed.get()); }
    catch (const std::runtime_error& error) { caught = std::string(error.what()) == "expected"; }
    require(caught && failed.status() == TaskStatus::Failed && !failed.cancel(), "failed state");
    auto nonstandard = pool.submit_task([]() -> int { throw 23; });
    bool integer = false;
    try { static_cast<void>(nonstandard.get()); }
    catch (int error) { integer = error == 23; }
    require(integer && nonstandard.status() == TaskStatus::Failed, "non-std failure state");
}

void close_cancel_pending_preserves_running_and_legacy_futures() {
    auto gate = std::make_shared<Gate>();
    std::atomic<unsigned> called{0};
    taskforge::ThreadPool pool(1);
    ReleaseOnExit release{gate};
    auto running = pool.submit_task([gate] { gate->enter_and_wait(); return 12; });
    require(gate->wait_entered(), "close test worker did not start");
    auto pending = pool.submit_task([&called] { ++called; });
    auto legacy = pool.submit([&called] { ++called; return 8; });
    pool.close(); // Drain can be escalated to CancelPending.
    pool.close(taskforge::ShutdownMode::CancelPending);
    pool.close(taskforge::ShutdownMode::CancelPending);
    require(pool.is_closed(), "pool did not close admission");
    require(pending.wait_for(0ms) == std::future_status::ready && get_cancelled(pending),
            "close must resolve pending handle");
    bool cancelled = false;
    try { static_cast<void>(legacy.get()); }
    catch (const taskforge::TaskCancelled&) { cancelled = true; }
    require(cancelled, "close must resolve legacy future with cancellation");
    require(running.status() == TaskStatus::Running && !running.cancellation_requested(),
            "CancelPending must not cancel dequeued/running tasks");
    bool rejected = false;
    try { static_cast<void>(pool.submit([] {})); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "closed pool accepted submit");
    gate->release();
    require(running.get() == 12, "running task lost during close");
    pool.wait();
    pool.wait();
    require(called.load() == 0, "close executed discarded queue tasks");
}

void drain_concurrent_waiters_and_worker_guard() {
    std::atomic<unsigned> count{0};
    taskforge::ThreadPool pool(3);
    std::vector<taskforge::TaskHandle<unsigned>> tasks;
    for (unsigned index = 0; index < 200; ++index) {
        tasks.push_back(pool.submit_task([&count, index] { ++count; return index; }));
    }
    auto guard = pool.submit_task([&pool] {
        unsigned rejected = 0;
        try { pool.wait(); } catch (const std::logic_error&) { ++rejected; }
        try { pool.shutdown(); } catch (const std::logic_error&) { ++rejected; }
        return rejected;
    });
    auto first = std::async(std::launch::async, [&pool] { pool.wait(); });
    auto second = std::async(std::launch::async, [&pool] { pool.shutdown(); });
    first.get();
    second.get();
    require(count.load() == 200 && guard.get() == 2, "drain/worker wait guard");
    for (unsigned index = 0; index < tasks.size(); ++index) {
        require(tasks[index].get() == index &&
                tasks[index].status() == TaskStatus::Succeeded, "drain lost a result");
    }
    taskforge::ThreadPool worker_close(1);
    auto closed = worker_close.submit([&worker_close] { worker_close.close(); return 1; });
    require(closed.get() == 1 && worker_close.is_closed(), "worker may close admission");
}

void cancellation_and_completion_race() {
    std::vector<std::atomic<unsigned>> invocations(400);
    for (auto& count : invocations) { count.store(0); }
    auto gate = std::make_shared<Gate>();
    taskforge::ThreadPool pool(4);
    ReleaseOnExit release{gate};
    std::vector<std::future<void>> blockers;
    for (unsigned index = 0; index < 4; ++index) {
        blockers.push_back(pool.submit([gate] { gate->enter_and_wait(); }));
    }
    require(gate->wait_entered_count(4), "race workers did not reach the start gate");
    std::vector<taskforge::TaskHandle<unsigned>> handles;
    handles.reserve(invocations.size());
    for (unsigned index = 0; index < invocations.size(); ++index) {
        handles.push_back(pool.submit_task([&invocations, index] {
            ++invocations[index];
            return index;
        }));
    }
    std::promise<void> start;
    auto go = start.get_future().share();
    auto canceller = std::async(std::launch::async, [&handles, go] {
        go.wait();
        for (auto& handle : handles) { handle.cancel(); }
    });
    start.set_value();
    gate->release();
    for (unsigned index = 0; index < handles.size(); ++index) {
        try {
            require(handles[index].get() == index, "race changed returned value");
            require(handles[index].status() == TaskStatus::Succeeded, "success status race");
        } catch (const taskforge::TaskCancelled&) {
            require(handles[index].status() == TaskStatus::Cancelled, "cancelled status race");
        }
    }
    canceller.get();
    for (auto& blocker : blockers) { blocker.get(); }
    pool.wait();
    for (const auto& count : invocations) {
        require(count.load() <= 1, "race executed a task more than once");
    }
}

void submission_and_close_race() {
    std::atomic<unsigned> executed{0};
    taskforge::ThreadPool pool(3);
    auto seed = pool.submit([&executed] { ++executed; });
    std::promise<void> start;
    auto go = start.get_future().share();
    std::vector<std::future<unsigned>> producers;
    for (unsigned producer = 0; producer < 4; ++producer) {
        producers.push_back(std::async(std::launch::async, [&pool, &executed, go] {
            go.wait();
            std::vector<std::future<void>> accepted;
            for (unsigned index = 0; index < 200; ++index) {
                try { accepted.push_back(pool.submit([&executed] { ++executed; })); }
                catch (const std::runtime_error&) { break; }
            }
            for (auto& result : accepted) { result.get(); }
            return static_cast<unsigned>(accepted.size());
        }));
    }
    start.set_value();
    pool.close();
    unsigned accepted = 1;
    for (auto& producer : producers) { accepted += producer.get(); }
    seed.get();
    pool.wait();
    require(executed.load() == accepted, "close raced with submit: lost/duplicated task");
}

void wav_cancellation_checkpoint_and_consistency(const std::filesystem::path& sample) {
    std::atomic<bool> checkpoint{false};
    taskforge::ThreadPool pool(1);
    auto normal = pool.submit_cancellable(taskforge::analyze_wav_cancellable, sample);
    const auto original = taskforge::analyze_wav(sample);
    const auto current = normal.get();
    require(current.rms == original.rms && current.energy == original.energy &&
            current.peak == original.peak && current.frames == original.frames,
            "cancellable audio changed sample statistics");
    auto gate = std::make_shared<Gate>();
    ReleaseOnExit release{gate};
    auto cancelled = pool.submit_cancellable(
        [gate, &checkpoint](taskforge::CancellationToken token) {
            gate->enter_and_wait();
            try {
                return taskforge::analyze_wav_cancellable(
                    token, std::filesystem::path("must_not_open_this_cancelled_input.wav"));
            } catch (const taskforge::TaskCancelled&) {
                checkpoint.store(true);
                throw;
            }
        });
    require(gate->wait_entered(), "audio checkpoint task did not start");
    require(cancelled.cancel(), "audio cancellation request");
    gate->release();
    require(get_cancelled(cancelled) && checkpoint.load(),
            "audio must check token before opening input");
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        require(argc == 2, "provide sample WAV path");
        queued_cancel_is_immediate_and_skips_callable();
        running_cancel_waits_for_callable_exit();
        noncooperative_cancel_does_not_kill_or_undo_work();
        timeout_is_only_a_wait_result();
        results_failures_and_moved_handles();
        close_cancel_pending_preserves_running_and_legacy_futures();
        drain_concurrent_waiters_and_worker_guard();
        cancellation_and_completion_race();
        submission_and_close_race();
        wav_cancellation_checkpoint_and_consistency(std::filesystem::u8path(argv[1]));
        std::cout << "Task states, queued/running cancellation, wait timeout, close/join, "
                     "races and WAV cancellation: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Task lifecycle test failed: " << error.what() << "\n";
        return 1;
    }
}
