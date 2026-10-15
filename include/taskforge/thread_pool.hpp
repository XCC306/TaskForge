#pragma once

#include "taskforge/task.hpp"

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

enum class ShutdownMode { Drain, CancelPending };

// Fixed workers and an unbounded FIFO queue. User code runs outside the pool lock.
// Owners must finish all calls using the pool before destruction. Never destroy
// the pool from one of its workers. All running callables must eventually return.
class ThreadPool {
public:
    explicit ThreadPool(std::size_t worker_count);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    std::size_t worker_count() const noexcept { return workers_.size(); }

    // Nonblocking admission close; running tasks continue in either mode.
    // A later CancelPending call can escalate an earlier Drain close.
    void close(ShutdownMode mode = ShutdownMode::Drain);
    bool is_closed() const;

    // wait() also closes admission in Drain mode, then joins every worker.
    // Both blocking methods reject calls from a worker of this same pool.
    void wait();
    void shutdown(ShutdownMode mode = ShutdownMode::Drain);

    // v0.2.0 source-compatible interface, without a task handle.
    template <class Function, class... Args>
    auto submit(Function&& function, Args&&... args)
        -> std::future<std::invoke_result_t<std::decay_t<Function>, std::decay_t<Args>...>> {
        auto handle = submit_task(std::forward<Function>(function),
                                  std::forward<Args>(args)...);
        return std::move(handle.future_);
    }

    // Callable and arguments are owned by value. Use std::ref for references.
    template <class Function, class... Args>
    auto submit_task(Function&& function, Args&&... args)
        -> TaskHandle<std::invoke_result_t<std::decay_t<Function>, std::decay_t<Args>...>> {
        using Result = std::invoke_result_t<std::decay_t<Function>, std::decay_t<Args>...>;
        auto invocation = [callable = std::forward<Function>(function),
                           arguments = std::make_tuple(std::forward<Args>(args)...)]()
            mutable -> Result {
            return std::apply(
                [&callable](auto&&... values) -> Result {
                    return std::invoke(std::move(callable),
                                       std::forward<decltype(values)>(values)...);
                }, std::move(arguments));
        };
        auto control = std::make_shared<detail::TaskControl<Result>>();
        return enqueue_invocation<Result>(std::move(control), std::move(invocation));
    }

    // Callable signature: R(CancellationToken, Args...). No forced thread kill.
    template <class Function, class... Args>
    auto submit_cancellable(Function&& function, Args&&... args)
        -> TaskHandle<std::invoke_result_t<std::decay_t<Function>, CancellationToken,
                                           std::decay_t<Args>...>> {
        using Result = std::invoke_result_t<std::decay_t<Function>, CancellationToken,
                                            std::decay_t<Args>...>;
        auto control = std::make_shared<detail::TaskControl<Result>>();
        auto invocation = [callable = std::forward<Function>(function),
                           arguments = std::make_tuple(std::forward<Args>(args)...),
                           token = control->token()]() mutable -> Result {
            return std::apply(
                [&callable, token](auto&&... values) -> Result {
                    return std::invoke(std::move(callable), token,
                                       std::forward<decltype(values)>(values)...);
                }, std::move(arguments));
        };
        return enqueue_invocation<Result>(std::move(control), std::move(invocation));
    }

private:
    struct QueuedTask {
        std::shared_ptr<detail::TaskControlBase> control;
        std::function<void()> execute;
    };

    template <class Result, class Invocation>
    TaskHandle<Result> enqueue_invocation(std::shared_ptr<detail::TaskControl<Result>> control,
                                         Invocation&& invocation) {
        auto future = control->get_future();
        // Copyable queue wrappers also support move-only user callables.
        auto owned = std::make_shared<std::decay_t<Invocation>>(
            std::forward<Invocation>(invocation));
        enqueue(QueuedTask{control, [control, owned] { control->execute(*owned); }});
        return TaskHandle<Result>(std::move(control), std::move(future));
    }

    void enqueue(QueuedTask task);
    void worker_loop();
    void reject_worker_wait() const;

    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::queue<QueuedTask> tasks_;
    bool closing_ = false;
    std::mutex join_mutex_;
    std::vector<std::thread> workers_;
};

} // namespace taskforge
