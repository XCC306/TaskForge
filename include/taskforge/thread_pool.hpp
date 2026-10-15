#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace taskforge {

// Fixed workers, FIFO submission queue, unbounded queue in v0.2.0.
// Tasks run outside the queue lock. Task exceptions are stored in their futures.
// Destruction waits for every accepted task and joins the worker threads.
// Owners must ensure submit calls have ended before destruction, and must not
// destroy the pool from a worker task. A task that never returns delays destruction.
class ThreadPool {
public:
    explicit ThreadPool(std::size_t worker_count);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    std::size_t worker_count() const noexcept { return workers_.size(); }

    // Callable/arguments are owned by value; use std::ref for explicit references.
    // Invokes each accepted callable once, moving its stored arguments into it.
    template <class Function, class... Args>
    auto submit(Function&& function, Args&&... args)
        -> std::future<std::invoke_result_t<std::decay_t<Function>, std::decay_t<Args>...>> {
        using Result = std::invoke_result_t<std::decay_t<Function>, std::decay_t<Args>...>;

        auto invocation = [callable = std::forward<Function>(function),
                           arguments = std::make_tuple(std::forward<Args>(args)...)]()
            mutable -> Result {
            return std::apply(
                [&callable](auto&&... values) -> Result {
                    return std::invoke(std::move(callable),
                                       std::forward<decltype(values)>(values)...);
                },
                std::move(arguments));
        };

        // shared_ptr makes the queue wrapper copyable even for move-only tasks.
        auto task = std::make_shared<std::packaged_task<Result()>>(std::move(invocation));
        auto result = task->get_future();
        enqueue([task] { (*task)(); });
        return result;
    }

private:
    void enqueue(std::function<void()> task);
    void worker_loop();
    void drain_and_join();

    std::mutex mutex_;
    std::condition_variable ready_;
    std::queue<std::function<void()>> tasks_;
    bool closing_ = false; // Protected by mutex_.
    std::vector<std::thread> workers_;
};

} // namespace taskforge
