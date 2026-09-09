#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace util {

// Task-queue thread pool with futures.
//
// The branch-and-bound driver submits one long-lived worker loop per thread
// rather than one task per node, so the pool must be able to run every
// submitted task concurrently; tasks that block waiting for work must not
// starve each other. Threads are therefore created up front and each runs
// tasks to completion.
class ThreadPool {
public:
    // 0 selects hardware_concurrency.
    explicit ThreadPool(std::size_t threads = 0);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    [[nodiscard]] std::size_t threadCount() const noexcept { return workers_.size(); }

    template <typename Callable>
    auto submit(Callable&& callable)
        -> std::future<typename std::invoke_result<Callable>::type> {
        using Result = typename std::invoke_result<Callable>::type;
        auto task = std::make_shared<std::packaged_task<Result()>>(
            std::forward<Callable>(callable));
        std::future<Result> future = task->get_future();
        {
            std::lock_guard<std::mutex> guard(mutex_);
            queue_.emplace([task]() { (*task)(); });
        }
        condition_.notify_one();
        return future;
    }

private:
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> queue_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool stopping_ = false;
};

}  // namespace util
