#include "taskforge/thread_pool.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& why) {
    if (!condition) {
        throw std::runtime_error(why);
    }
}

void zero_workers_rejected() {
    bool rejected = false;
    try {
        taskforge::ThreadPool pool(0);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "zero workers must be rejected");
}

void results_and_owned_arguments() {
    taskforge::ThreadPool pool(2);
    require(pool.worker_count() == 2, "worker count");
    auto sum = pool.submit([](int left, int right) { return left + right; }, 3, 4);
    require(sum.get() == 7, "value result");

    auto moved = pool.submit(
        [first = std::make_unique<int>(5)](std::unique_ptr<int> second) {
            return *first + *second;
        },
        std::make_unique<int>(8));
    require(moved.get() == 13, "move-only callable and argument");

    int value = 0;
    auto changed = pool.submit([](int& number) { number = 9; }, std::ref(value));
    changed.get();
    require(value == 9, "void result and explicit reference");
}

void exceptions_do_not_stop_workers() {
    taskforge::ThreadPool pool(1);
    auto failed = pool.submit([]() -> int { throw std::runtime_error("task failure"); });
    bool received = false;
    try {
        static_cast<void>(failed.get());
    } catch (const std::runtime_error& error) {
        received = std::string(error.what()) == "task failure";
    }
    require(received, "future must rethrow task exception");

    auto nonstandard = pool.submit([]() -> int { throw 42; });
    bool received_integer = false;
    try {
        static_cast<void>(nonstandard.get());
    } catch (int value) {
        received_integer = value == 42;
    }
    require(received_integer, "future must preserve non-std exception");
    require(pool.submit([] { return 17; }).get() == 17, "worker survived failures");
}

struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    std::size_t arrived = 0;
    bool released = false;

    void release() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            released = true;
        }
        changed.notify_all();
    }
};

// Release waiting tasks even if an assertion or submission throws.
struct ReleaseOnExit {
    std::shared_ptr<Gate> gate;
    ~ReleaseOnExit() { gate->release(); }
};

void workers_execute_concurrently() {
    constexpr std::size_t worker_count = 4;
    auto gate = std::make_shared<Gate>();
    taskforge::ThreadPool pool(worker_count);
    ReleaseOnExit release{gate};
    std::vector<std::future<std::thread::id>> results;
    for (std::size_t index = 0; index < worker_count; ++index) {
        results.push_back(pool.submit([gate] {
            std::unique_lock<std::mutex> lock(gate->mutex);
            ++gate->arrived;
            gate->changed.notify_all();
            gate->changed.wait(lock, [gate] { return gate->released; });
            return std::this_thread::get_id();
        }));
    }
    bool all_arrived = false;
    {
        std::unique_lock<std::mutex> lock(gate->mutex);
        all_arrived = gate->changed.wait_for(
            lock, std::chrono::seconds(5),
            [gate, expected_count = worker_count] {
    return gate->arrived == expected_count;
});
    }
    gate->release();
    std::set<std::thread::id> workers;
    for (auto& result : results) {
        workers.insert(result.get());
    }
    require(all_arrived && workers.size() == worker_count,
            "all four workers must enter independent tasks before release");
}

void multiple_producers_execute_each_task_once() {
    constexpr std::size_t producers = 4;
    constexpr std::size_t per_producer = 250;
    constexpr std::size_t total = producers * per_producer;
    std::vector<std::atomic<unsigned>> counts(total);
    for (auto& count : counts) {
        count.store(0);
    }
    taskforge::ThreadPool pool(4);
    using Results = std::vector<std::future<std::size_t>>;
    std::vector<std::future<Results>> submitted;
    for (std::size_t producer = 0; producer < producers; ++producer) {
        submitted.push_back(std::async(std::launch::async, [&, producer] {
            Results results;
            results.reserve(per_producer);
            for (std::size_t index = 0; index < per_producer; ++index) {
                const std::size_t task_index = producer * per_producer + index;
                results.push_back(pool.submit([&, task_index] {
                    counts[task_index].fetch_add(1);
                    return task_index;
                }));
            }
            return results;
        }));
    }
    for (std::size_t producer = 0; producer < producers; ++producer) {
        auto results = submitted[producer].get();
        for (std::size_t index = 0; index < per_producer; ++index) {
            require(results[index].get() == producer * per_producer + index,
                    "future associated with wrong task");
        }
    }
    for (const auto& count : counts) {
        require(count.load() == 1, "accepted task lost or duplicated");
    }
}

void destruction_drains_accepted_tasks() {
    std::atomic<unsigned> completed{0};
    std::vector<std::future<unsigned>> results;
    {
        taskforge::ThreadPool pool(2);
        for (unsigned index = 0; index < 500; ++index) {
            results.push_back(pool.submit([&completed, index] {
                completed.fetch_add(1);
                return index;
            }));
        }
        // No future.get here: destruction itself must wait for accepted tasks.
    }
    require(completed.load() == 500, "destructor did not drain the queue");
    for (unsigned index = 0; index < results.size(); ++index) {
        require(results[index].get() == index, "future survives pool destruction");
    }
    { taskforge::ThreadPool empty(2); }
}

} // namespace

int main() {
    try {
        zero_workers_rejected();
        results_and_owned_arguments();
        exceptions_do_not_stop_workers();
        workers_execute_concurrently();
        multiple_producers_execute_each_task_once();
        destruction_drains_accepted_tasks();
        std::cout << "ThreadPool results, ownership, exceptions, concurrency, "
                     "1000-task submission and drain: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ThreadPool test failed: " << error.what() << "\n";
        return 1;
    }
}
