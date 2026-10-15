#include "taskforge/thread_pool.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace {
using namespace std::chrono_literals;

struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;
    void block() {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        changed.notify_all();
        changed.wait(lock, [this] { return released; });
    }
    void await_entry() {
        std::unique_lock<std::mutex> lock(mutex);
        if (!changed.wait_for(lock, 5s, [this] { return entered; })) {
            throw std::runtime_error("Worker did not start within 5 seconds");
        }
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

void require(bool condition) {
    if (!condition) { throw std::runtime_error("Lifecycle demonstration failed"); }
}
template <class T>
void expect_cancelled(taskforge::TaskHandle<T>& task) {
    bool cancelled = false;
    try { static_cast<void>(task.get()); }
    catch (const taskforge::TaskCancelled&) { cancelled = true; }
    require(cancelled && task.status() == taskforge::TaskStatus::Cancelled);
}
} // namespace

int main() {
    try {
        std::cout << "TaskForge v0.3.0 lifecycle demo\n";
        {
            auto gate = std::make_shared<Gate>();
            std::atomic<bool> called{false};
            taskforge::ThreadPool pool(1);
            ReleaseOnExit release{gate};
            auto running = pool.submit_task([gate] { gate->block(); return 42; });
            gate->await_entry();
            std::cout << "running task: " << taskforge::to_string(running.status()) << "\n";
            auto queued = pool.submit_task([&called] { called.store(true); });
            std::cout << "pending task: " << taskforge::to_string(queued.status()) << "\n";
            require(queued.cancel());
            expect_cancelled(queued);
            std::cout << "pending cancellation: cancelled (callable skipped)\n";
            require(running.wait_for(10ms) == std::future_status::timeout);
            require(!running.cancellation_requested());
            std::cout << "wait_for: timeout (task still running, no cancellation)\n";
            gate->release();
            const auto value = running.get();
            require(value == 42);
            pool.shutdown();
            require(!called.load());
            std::cout << "later result: 42; state=succeeded\n";
        }
        {
            auto gate = std::make_shared<Gate>();
            taskforge::ThreadPool pool(1);
            ReleaseOnExit release{gate};
            auto running = pool.submit_cancellable([gate](taskforge::CancellationToken token) {
                gate->block();
                token.throw_if_cancelled();
            });
            gate->await_entry();
            require(running.cancel());
            require(running.status() == taskforge::TaskStatus::Running);
            std::cout << "running cancellation: requested (state remains running until exit)\n";
            gate->release();
            expect_cancelled(running);
            std::cout << "cooperative exit: cancelled\n";
        }
        {
            auto gate = std::make_shared<Gate>();
            taskforge::ThreadPool pool(1);
            ReleaseOnExit release{gate};
            auto running = pool.submit_task([gate] { gate->block(); return 1; });
            gate->await_entry();
            auto pending = pool.submit_task([] { return 2; });
            pool.close(taskforge::ShutdownMode::CancelPending);
            expect_cancelled(pending);
            require(running.status() == taskforge::TaskStatus::Running);
            gate->release();
            require(running.get() == 1);
            pool.wait();
            std::cout << "CancelPending close: queued cancelled; running finished; workers joined\n";
        }
        std::cout << "Lifecycle demo: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
