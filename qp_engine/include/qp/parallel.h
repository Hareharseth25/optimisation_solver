#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

namespace qp {

// Persistent worker pool, same design as the one in pdlp_engine.
//
// ADMM dispatches a handful of short parallel regions per iteration, so a
// fork/join pool would spend more time creating threads than doing work.
// Workers stay alive for the lifetime of the Executor and meet at a
// spin-then-yield barrier; the submitting thread runs part 0 and participates.
class Executor {
public:
    // requestedThreads <= 0 selects hardware_concurrency().
    explicit Executor(int requestedThreads);
    ~Executor();

    Executor(const Executor&) = delete;
    Executor& operator=(const Executor&) = delete;

    [[nodiscard]] int threadCount() const noexcept { return threadCount_; }

    void run(const std::function<void(int)>& body);

private:
    void workerLoop(int index);

    std::vector<std::thread> workers_;
    const std::function<void(int)>* body_ = nullptr;
    std::atomic<std::uint64_t> generation_{0};
    std::atomic<int> pending_{0};
    std::atomic<bool> stopping_{false};
    int threadCount_ = 1;
};

}  // namespace qp
