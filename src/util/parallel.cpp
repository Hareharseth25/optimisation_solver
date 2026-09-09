#include "util/parallel.h"

#include <algorithm>

namespace util {

ThreadPool::ThreadPool(std::size_t threads) {
    std::size_t count = threads;
    if (count == 0) {
        count = static_cast<std::size_t>(std::thread::hardware_concurrency());
    }
    count = std::max<std::size_t>(count, 1);

    workers_.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        workers_.emplace_back([this] {
            while (true) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    condition_.wait(lock, [this] {
                        return stopping_ || !queue_.empty();
                    });
                    if (stopping_ && queue_.empty()) {
                        return;
                    }
                    task = std::move(queue_.front());
                    queue_.pop();
                }
                task();
            }
        });
    }
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> guard(mutex_);
        stopping_ = true;
    }
    condition_.notify_all();
    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

}  // namespace util
