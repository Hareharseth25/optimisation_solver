#pragma once

#include "pdlp/parallel.h"
#include "pdlp/pdlp_state.h"

#include <vector>

namespace pdlp {

// Running weighted mean of the iterate sequence.
//
// Stored incrementally (avg += w/W * (x - avg)) rather than as a running sum
// divided at the end: the sum form loses precision once the accumulated weight
// spans many orders of magnitude, which it does across a long restart interval.
class IterateAverage {
public:
    IterateAverage(
        int numColumns,
        int numRows,
        Executor* executor = nullptr
    );

    void add(
        const std::vector<double>& primal,
        const std::vector<double>& dual,
        double weight
    );

    [[nodiscard]] const CandidateIterate& candidate() const;
    [[nodiscard]] bool empty() const noexcept { return totalWeight_ == 0.0; }
    [[nodiscard]] double totalWeight() const noexcept { return totalWeight_; }

    void reset();

private:
    CandidateIterate average_;
    double totalWeight_ = 0.0;
    Executor* executor_ = nullptr;
    int parts_ = 1;
};

}  // namespace pdlp
