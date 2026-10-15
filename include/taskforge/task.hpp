#pragma once

#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace taskforge {

enum class TaskStatus { Queued, Running, Succeeded, Failed, Cancelled };

inline const char* to_string(TaskStatus status) noexcept {
    switch (status) {
    case TaskStatus::Queued: return "queued";
    case TaskStatus::Running: return "running";
    case TaskStatus::Succeeded: return "succeeded";
    case TaskStatus::Failed: return "failed";
    case TaskStatus::Cancelled: return "cancelled";
    }
    return "unknown";
}

class TaskCancelled : public std::runtime_error {
public:
    TaskCancelled() : std::runtime_error("Task cancelled") {}
};

namespace detail { class TaskControlBase; }

// Copyable, read-only stop signal. A default token never requests cancellation.
class CancellationToken {
public:
    CancellationToken() = default;
    bool stop_requested() const noexcept {
        return requested_ && requested_->load(std::memory_order_relaxed);
    }
    void throw_if_cancelled() const {
        if (stop_requested()) {
            throw TaskCancelled();
        }
    }
private:
    friend class detail::TaskControlBase;
    explicit CancellationToken(std::shared_ptr<std::atomic<bool>> requested)
        : requested_(std::move(requested)) {}
    std::shared_ptr<std::atomic<bool>> requested_;
};

namespace detail {

// The same task mutex protects every transition and promise publication.
// Running cancellation is a request, not an immediately published terminal state.
class TaskControlBase {
public:
    virtual ~TaskControlBase() = default;
    TaskStatus status() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return status_;
    }
    CancellationToken token() const { return CancellationToken(requested_); }

    bool request_cancel() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (terminal_locked()) {
            return false;
        }
        const bool first_request = !requested_->exchange(true, std::memory_order_relaxed);
        if (status_ == TaskStatus::Queued) {
            publish_cancelled_locked();
            status_ = TaskStatus::Cancelled;
        }
        return first_request;
    }

    // Only for entries removed from the pending queue by close(CancelPending).
    void cancel_if_queued() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_ == TaskStatus::Queued) {
            requested_->store(true, std::memory_order_relaxed);
            publish_cancelled_locked();
            status_ = TaskStatus::Cancelled;
        }
    }
protected:
    bool begin() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_ != TaskStatus::Queued) {
            return false;
        }
        status_ = TaskStatus::Running;
        return true;
    }
    bool terminal_locked() const noexcept {
        return status_ == TaskStatus::Succeeded || status_ == TaskStatus::Failed ||
               status_ == TaskStatus::Cancelled;
    }
    virtual void publish_cancelled_locked() = 0;
    mutable std::mutex mutex_;
    TaskStatus status_ = TaskStatus::Queued;
    std::shared_ptr<std::atomic<bool>> requested_ =
        std::make_shared<std::atomic<bool>>(false);
};

template <class Result>
class TaskControl final : public TaskControlBase {
public:
    std::future<Result> get_future() { return promise_.get_future(); }

    template <class Invocation>
    void execute(Invocation& invocation) {
        if (!begin()) {
            return; // Queued cancellation already made its future ready.
        }
        try {
            if constexpr (std::is_void_v<Result>) {
                invocation();
                finish_success([this] { promise_.set_value(); });
            } else {
                decltype(auto) value = invocation();
                finish_success([this, &value] {
                    promise_.set_value(std::forward<Result>(value));
                });
            }
        } catch (const TaskCancelled&) {
            std::lock_guard<std::mutex> lock(mutex_);
            requested_->store(true, std::memory_order_relaxed);
            publish_cancelled_locked();
            status_ = TaskStatus::Cancelled;
        } catch (...) {
            const auto failure = std::current_exception();
            std::lock_guard<std::mutex> lock(mutex_);
            if (requested_->load(std::memory_order_relaxed)) {
                publish_cancelled_locked();
                status_ = TaskStatus::Cancelled;
            } else {
                promise_.set_exception(failure);
                status_ = TaskStatus::Failed;
            }
        }
    }
private:
    template <class Publish>
    void finish_success(Publish&& publish) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (requested_->load(std::memory_order_relaxed)) {
            publish_cancelled_locked();
            status_ = TaskStatus::Cancelled;
        } else {
            publish();
            status_ = TaskStatus::Succeeded;
        }
    }
    void publish_cancelled_locked() override {
        promise_.set_exception(std::make_exception_ptr(TaskCancelled()));
    }
    std::promise<Result> promise_;
};

} // namespace detail

class ThreadPool;

// Move-only consumer handle. status()/cancel() remain usable after get().
// status()/cancel() may run alongside one consumer operation. Do not move/destroy
// the same handle concurrently, or call get()/wait() concurrently with get().
template <class Result>
class TaskHandle {
public:
    TaskHandle() = default;
    TaskHandle(TaskHandle&&) noexcept = default;
    TaskHandle& operator=(TaskHandle&&) noexcept = default;
    TaskHandle(const TaskHandle&) = delete;
    TaskHandle& operator=(const TaskHandle&) = delete;

    bool valid() const noexcept { return future_.valid(); }
    TaskStatus status() const { return control().status(); }
    // True only for the first accepted request. Terminal tasks are unchanged.
    bool cancel() { return control().request_cancel(); }
    bool cancellation_requested() const { return control().token().stop_requested(); }

    template <class Rep, class Period>
    std::future_status wait_for(const std::chrono::duration<Rep, Period>& duration) const {
        return future_.wait_for(duration); // Timeout never changes task state.
    }
    void wait() const { future_.wait(); }
    Result get() { return future_.get(); }

private:
    friend class ThreadPool;
    TaskHandle(std::shared_ptr<detail::TaskControl<Result>> control,
               std::future<Result> future)
        : control_(std::move(control)), future_(std::move(future)) {}
    detail::TaskControl<Result>& control() const {
        if (!control_) {
            throw std::future_error(std::future_errc::no_state);
        }
        return *control_;
    }
    std::shared_ptr<detail::TaskControl<Result>> control_;
    std::future<Result> future_;
};

} // namespace taskforge
