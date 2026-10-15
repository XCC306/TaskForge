#include "taskforge/thread_pool.hpp"

#include <stdexcept>

namespace taskforge {

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
        // If thread creation fails midway, wake and join the threads already made.
        drain_and_join();
        throw;
    }
}

ThreadPool::~ThreadPool() {
    drain_and_join();
}

void ThreadPool::enqueue(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closing_) {
            throw std::runtime_error("ThreadPool is closing");
        }
        tasks_.push(std::move(task));
    }
    ready_.notify_one();
}

void ThreadPool::worker_loop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ready_.wait(lock, [this] { return closing_ || !tasks_.empty(); });
            if (closing_ && tasks_.empty()) {
                return;
            }
            task = std::move(tasks_.front());
            tasks_.pop();
        }
        // packaged_task catches user exceptions and stores them in the future.
        task();
    }
}

void ThreadPool::drain_and_join() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closing_ = true;
    }
    ready_.notify_all();
    // No queue lock is held while waiting: workers need it to finish the queue.
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

} // namespace taskforge
