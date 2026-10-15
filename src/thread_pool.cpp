#include "taskforge/thread_pool.hpp"

#include <stdexcept>

namespace taskforge {
namespace {
thread_local const ThreadPool* current_pool = nullptr;

struct WorkerScope {
    explicit WorkerScope(const ThreadPool* pool) { current_pool = pool; }
    ~WorkerScope() { current_pool = nullptr; }
};
} // namespace

ThreadPool::ThreadPool(std::size_t worker_count) {
    if (worker_count == 0) {
        throw std::invalid_argument("ThreadPool requires at least one worker");
    }
    workers_.reserve(worker_count);
    try {
        for (std::size_t index = 0; index < worker_count; ++index) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    } catch (...) {
        wait(); // Wake and join threads already made if construction fails.
        throw;
    }
}

ThreadPool::~ThreadPool() {
    wait(); // Same drain default as v0.2.0. Never detach live tasks.
}

bool ThreadPool::is_closed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closing_;
}

void ThreadPool::enqueue(QueuedTask task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closing_) {
            throw std::runtime_error("ThreadPool is closed");
        }
        tasks_.push(std::move(task));
    }
    ready_.notify_one();
}

void ThreadPool::close(ShutdownMode mode) {
    std::queue<QueuedTask> cancelled;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closing_ = true;
        if (mode == ShutdownMode::CancelPending) {
            tasks_.swap(cancelled);
        }
    }
    ready_.notify_all();
    // No promise publication or destruction of user captures under the pool lock.
    while (!cancelled.empty()) {
        cancelled.front().control->cancel_if_queued();
        cancelled.pop();
    }
}

void ThreadPool::reject_worker_wait() const {
    if (current_pool == this) {
        throw std::logic_error("A worker cannot wait for its own ThreadPool");
    }
}

void ThreadPool::wait() {
    reject_worker_wait(); // Before join_mutex_: do not deadlock with a joining owner.
    close();
    std::lock_guard<std::mutex> lock(join_mutex_);
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void ThreadPool::shutdown(ShutdownMode mode) {
    reject_worker_wait();
    close(mode);
    wait();
}

void ThreadPool::worker_loop() {
    WorkerScope scope(this);
    for (;;) {
        QueuedTask task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ready_.wait(lock, [this] { return closing_ || !tasks_.empty(); });
            if (closing_ && tasks_.empty()) {
                return;
            }
            task = std::move(tasks_.front());
            tasks_.pop();
        }
        task.execute(); // TaskControl stores values, cancellation, or exceptions.
    }
}

} // namespace taskforge
