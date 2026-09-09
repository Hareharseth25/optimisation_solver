#pragma once

#include <cstdint>
#include <vector>

namespace pdlp {

struct PdlpState {
    std::vector<double> primal;       // x^k
    std::vector<double> dual;         // y^k

    // A*x^k, carried across iterations.
    //
    // The extrapolated activity the dual step needs is A*x_bar = 2*A*x' - A*x^k,
    // and the linesearch needs A*dx = A*x' - A*x^k. Keeping A*x^k means the
    // iteration computes one sparse product (A*x') and recovers both by
    // subtraction, so the linesearch's interaction term costs no extra pass over
    // the matrix. On commit this is replaced by the freshly computed A*x', not
    // accumulated into, so it carries no drift.
    std::vector<double> rowActivity;

    std::int64_t iteration = 0;
};

struct CandidateIterate {
    std::vector<double> primal;
    std::vector<double> dual;
};

struct DiagonalPreconditioner {
    std::vector<double> primalScale;   // T_j
    std::vector<double> dualScale;     // Sigma_i

    // Reciprocals, precomputed: the linesearch measures movement in the
    // inverse-preconditioner norm and would otherwise divide per coordinate.
    std::vector<double> primalScaleInverse;
    std::vector<double> dualScaleInverse;
};

struct StepParameters {
    double globalStep = 1.0;      // eta
    double primalWeight = 1.0;    // omega; primal step = eta/omega, dual = eta*omega
    double maximumSafeGlobalStep = 1.0;
};

}  // namespace pdlp
