#include "pdlp/parallel.h"

#include <algorithm>

namespace pdlp {
namespace {

constexpr int kSpinLimit = 2048;

inline void spinPause() noexcept {
#if defined(__aarch64__) || defined(_M_ARM64)
    __asm__ __volatile__("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    std::this_thread::yield();
#endif
}

}  // namespace

Executor::Executor(int requestedThreads) {
    int count = requestedThreads;
    if (count <= 0) {
        count = static_cast<int>(std::thread::hardware_concurrency());
    }
    threadCount_ = std::max(count, 1);

    workers_.reserve(static_cast<std::size_t>(threadCount_ - 1));
    for (int index = 1; index < threadCount_; ++index) {
        workers_.emplace_back([this, index] { workerLoop(index); });
    }
}

Executor::~Executor() {
    stopping_.store(true, std::memory_order_release);
    generation_.fetch_add(1, std::memory_order_release);
    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void Executor::workerLoop(int index) {
    std::uint64_t seen = 0;
    while (true) {
        int spins = 0;
        while (true) {
            if (stopping_.load(std::memory_order_acquire)) {
                return;
            }
            const std::uint64_t current = generation_.load(std::memory_order_acquire);
            if (current != seen) {
                seen = current;
                break;
            }
            if (spins < kSpinLimit) {
                ++spins;
                spinPause();
            } else {
                std::this_thread::yield();
            }
        }

        // Re-check after waking: shutdown also bumps the generation, and body_
        // is not valid in that case.
        if (stopping_.load(std::memory_order_acquire)) {
            return;
        }
        (*body_)(index);
        pending_.fetch_sub(1, std::memory_order_release);
    }
}

void Executor::run(const std::function<void(int)>& body) {
    if (threadCount_ <= 1) {
        body(0);
        return;
    }

    body_ = &body;
    pending_.store(threadCount_ - 1, std::memory_order_relaxed);
    generation_.fetch_add(1, std::memory_order_release);

    body(0);

    int spins = 0;
    while (pending_.load(std::memory_order_acquire) != 0) {
        if (spins < kSpinLimit) {
            ++spins;
            spinPause();
        } else {
            std::this_thread::yield();
        }
    }
}

}  // namespace pdlp
