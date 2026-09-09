#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

namespace pdlp {

// A persistent, dependency-free worker pool.
//
// The solver dispatches several short parallel regions per PDHG iteration, so
// the barrier cost dominates any fork/join design: creating threads per region
// would cost more than the region itself. Workers therefore stay alive for the
// lifetime of the Executor and synchronise through a spin-then-yield barrier.
// The submitting thread executes part 0 and participates in the work.
class Executor {
public:
    // requestedThreads <= 0 selects std::thread::hardware_concurrency().
    explicit Executor(int requestedThreads);
    ~Executor();

    Executor(const Executor&) = delete;
    Executor& operator=(const Executor&) = delete;

    [[nodiscard]] int threadCount() const noexcept { return threadCount_; }

    // Invokes body(part) for every part in [0, threadCount).
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

// Half-open range [begin, end) of `count` items assigned to `part` of `parts`.
struct Range {
    std::int64_t begin = 0;
    std::int64_t end = 0;
};

[[nodiscard]] inline Range evenRange(std::int64_t count, int parts, int part) noexcept {
    const std::int64_t base = count / parts;
    const std::int64_t remainder = count % parts;
    const std::int64_t begin = base * part + (part < remainder ? part : remainder);
    const std::int64_t length = base + (part < remainder ? 1 : 0);
    return Range{begin, begin + length};
}

}  // namespace pdlp
