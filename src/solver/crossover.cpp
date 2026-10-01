#include "solver/crossover.h"

#include "milp/dual_simplex_solver.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>

namespace solver {
namespace {

constexpr double kInfinity = std::numeric_limits<double>::infinity();

double distanceToBound(double value, double lower, double upper) {
    double distance = kInfinity;
    if (std::isfinite(lower)) distance = std::min(distance, std::abs(value - lower));
    if (std::isfinite(upper)) distance = std::min(distance, std::abs(upper - value));
    return distance;
}

milp::BasisStatus nonbasicStatus(double value, double lower, double upper) {
    const bool hasLower = std::isfinite(lower), hasUpper = std::isfinite(upper);
    if (hasLower && hasUpper && lower == upper) return milp::BasisStatus::Fixed;
    if (hasLower && hasUpper)
        return std::abs(value - lower) <= std::abs(upper - value) ? milp::BasisStatus::AtLower
                                                                  : milp::BasisStatus::AtUpper;
    if (hasLower) return milp::BasisStatus::AtLower;
    if (hasUpper) return milp::BasisStatus::AtUpper;
    return milp::BasisStatus::Free;
}

// Greedy selection of linearly independent columns by Gaussian elimination
// with row pivoting. Each accepted column is stored reduced against every
// earlier one and normalised to 1 on its pivot row, so testing a new column
// costs one pass over the stored pivots. Dense in m, which is acceptable
// because crossover only runs inside the dense dual simplex's own size limits.
class IndependentColumns {
public:
    explicit IndependentColumns(int rows) : rows_(rows), pivotRow_(static_cast<std::size_t>(rows), 0) {}

    // `column` is a sparse (row, value) list. Returns true and keeps the
    // column if it is independent of those already kept.
    bool tryAdd(const std::vector<std::pair<int, double>>& column) {
        std::vector<double> v(static_cast<std::size_t>(rows_), 0.0);
        double scale = 0.0;
        for (const auto& [row, value] : column) {
            v[static_cast<std::size_t>(row)] += value;
            scale = std::max(scale, std::abs(value));
        }
        if (scale == 0.0) return false;
        for (std::size_t k = 0; k < pivots_.size(); ++k) {
            const double factor = v[static_cast<std::size_t>(rowOf_[k])];
            if (factor == 0.0) continue;
            const auto& w = pivots_[k];
            for (int r = 0; r < rows_; ++r) v[static_cast<std::size_t>(r)] -= factor * w[static_cast<std::size_t>(r)];
        }
        int best = -1;
        double bestValue = 0.0;
        for (int r = 0; r < rows_; ++r) {
            if (pivotRow_[static_cast<std::size_t>(r)]) continue;
            const double magnitude = std::abs(v[static_cast<std::size_t>(r)]);
            if (magnitude > bestValue) { bestValue = magnitude; best = r; }
        }
        // Relative threshold: a column that only survives elimination as
        // roundoff would make the basis matrix numerically singular.
        if (best < 0 || bestValue <= 1e-7 * scale) return false;
        const double pivot = v[static_cast<std::size_t>(best)];
        for (double& value : v) value /= pivot;
        pivots_.push_back(std::move(v));
        rowOf_.push_back(best);
        pivotRow_[static_cast<std::size_t>(best)] = 1;
        return true;
    }

    [[nodiscard]] int size() const noexcept { return static_cast<int>(pivots_.size()); }

private:
    int rows_;
    std::vector<std::vector<double>> pivots_;
    std::vector<int> rowOf_;
    std::vector<char> pivotRow_;
};

}  // namespace

CrossoverResult crossoverToVertex(const model::Model& model,
                                  const std::vector<double>& x,
                                  const std::vector<double>& y,
                                  double interiorObjective,
                                  const SolverOptions& options) {
    CrossoverResult out;
    const int n = static_cast<int>(model.variables.size());
    const int m = static_cast<int>(model.constraints.size());

    if (!model.objective.quadraticTerms.empty()) {
        out.detail = "not applied: crossover targets a vertex, which a quadratic objective need not have";
        return out;
    }
    if (m == 0) { out.detail = "not applied: no constraints"; return out; }

    // options.timeLimitSeconds is the budget LEFT for crossover (the caller
    // has already charged the barrier solve). 0 means no limit. Both basis
    // selection and the simplex cleanup are charged to it.
    const auto started = std::chrono::steady_clock::now();
    const auto remaining = [&] {
        return options.timeLimitSeconds -
               std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    };
    const bool limited = options.timeLimitSeconds > 0.0;
    const char* const outOfTime = "not applied: time limit reached during crossover";
    if (limited && remaining() <= 0.0) { out.detail = outOfTime; return out; }
    if (x.size() != static_cast<std::size_t>(n) || y.size() != static_cast<std::size_t>(m)) {
        out.detail = "not applied: interior solution is incomplete";
        return out;
    }
    std::int64_t nonzeros = 0;
    for (const auto& c : model.constraints) nonzeros += static_cast<std::int64_t>(c.linearTerms.size());
    if (static_cast<std::size_t>(m) >= options.dualSimplexMaxRows ||
        nonzeros > options.dualSimplexMaxNonzeros) {
        out.detail = "not applied: model exceeds the dense dual simplex's size limits";
        return out;
    }

    // Row activities, and reduced costs d = c - A'y. Only magnitudes are used
    // for ranking, so the objective sense does not matter here.
    std::vector<double> activity(static_cast<std::size_t>(m), 0.0);
    std::vector<double> reduced(static_cast<std::size_t>(n), 0.0);
    std::vector<std::vector<std::pair<int, double>>> column(static_cast<std::size_t>(n));
    for (const auto& term : model.objective.linearTerms)
        reduced[static_cast<std::size_t>(term.variableIndex)] += term.value;
    for (int i = 0; i < m; ++i)
        for (const auto& term : model.constraints[static_cast<std::size_t>(i)].linearTerms) {
            const auto j = static_cast<std::size_t>(term.variableIndex);
            activity[static_cast<std::size_t>(i)] += term.value * x[j];
            reduced[j] -= term.value * y[static_cast<std::size_t>(i)];
            column[j].emplace_back(i, term.value);
        }

    // Rank every column: distance to its nearest bound over its multiplier
    // magnitude. Near optimality complementarity drives one factor to ~mu and
    // leaves the other O(1), so active and inactive columns separate by many
    // orders of magnitude. A free column has no bound to sit on: always basic.
    struct Candidate { bool logical; int index; double score; };
    std::vector<Candidate> candidates;
    candidates.reserve(static_cast<std::size_t>(n + m));
    const auto score = [](double distance, double multiplier) {
        if (!std::isfinite(distance)) return kInfinity;
        return distance / std::max(std::abs(multiplier), 1e-300);
    };
    for (int j = 0; j < n; ++j) {
        const auto& v = model.variables[static_cast<std::size_t>(j)];
        if (v.lowerBound == v.upperBound) continue;  // fixed: nonbasic
        candidates.push_back({false, j, score(distanceToBound(x[static_cast<std::size_t>(j)], v.lowerBound, v.upperBound),
                                              reduced[static_cast<std::size_t>(j)])});
    }
    for (int i = 0; i < m; ++i) {
        const auto& c = model.constraints[static_cast<std::size_t>(i)];
        if (c.lowerBound == c.upperBound) continue;  // equality logical: nonbasic
        candidates.push_back({true, i, score(distanceToBound(activity[static_cast<std::size_t>(i)], c.lowerBound, c.upperBound),
                                             y[static_cast<std::size_t>(i)])});
    }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate& a, const Candidate& b) { return a.score > b.score; });

    IndependentColumns basis(m);
    std::vector<char> variableBasic(static_cast<std::size_t>(n), 0), rowBasic(static_cast<std::size_t>(m), 0);
    std::size_t examined = 0;
    for (const auto& candidate : candidates) {
        if (basis.size() == m) break;
        // Each independence test costs O(m * basis size); check the budget
        // every 64 candidates rather than on every one.
        if (limited && (++examined & 63u) == 0 && remaining() <= 0.0) { out.detail = outOfTime; return out; }
        if (candidate.logical) {
            // The logical of row i is -e_i in [A | -I]; its sign cannot affect
            // independence.
            if (basis.tryAdd({{candidate.index, -1.0}})) {
                rowBasic[static_cast<std::size_t>(candidate.index)] = 1;
                ++out.basicLogical;
            }
        } else if (basis.tryAdd(column[static_cast<std::size_t>(candidate.index)])) {
            variableBasic[static_cast<std::size_t>(candidate.index)] = 1;
            ++out.basicStructural;
        }
    }
    // Complete with any remaining logicals, equality rows included. [A | I]
    // has full row rank, so this always reaches m.
    for (int i = 0; i < m && basis.size() < m; ++i) {
        if (rowBasic[static_cast<std::size_t>(i)]) continue;
        if (basis.tryAdd({{i, -1.0}})) {
            rowBasic[static_cast<std::size_t>(i)] = 1;
            ++out.basicLogical;
        }
    }
    if (basis.size() != m) {
        out.detail = "not applied: could not complete a nonsingular basis";
        return out;
    }

    milp::BasisState state;
    state.variableStatus.resize(static_cast<std::size_t>(n));
    state.constraintStatus.resize(static_cast<std::size_t>(m));
    for (int j = 0; j < n; ++j) {
        const auto& v = model.variables[static_cast<std::size_t>(j)];
        state.variableStatus[static_cast<std::size_t>(j)] =
            variableBasic[static_cast<std::size_t>(j)] ? milp::BasisStatus::Basic
                : nonbasicStatus(x[static_cast<std::size_t>(j)], v.lowerBound, v.upperBound);
    }
    for (int i = 0; i < m; ++i) {
        const auto& c = model.constraints[static_cast<std::size_t>(i)];
        state.constraintStatus[static_cast<std::size_t>(i)] =
            rowBasic[static_cast<std::size_t>(i)] ? milp::BasisStatus::Basic
                : nonbasicStatus(activity[static_cast<std::size_t>(i)], c.lowerBound, c.upperBound);
    }

    milp::DualSimplexOptions simplexOptions;
    simplexOptions.primalFeasibilityTolerance = options.tolerance;
    simplexOptions.dualFeasibilityTolerance = options.tolerance;
    // The simplex gets what basis selection left. Never pass it a remainder
    // of zero or less: for the dual simplex, 0 means unlimited.
    if (limited) {
        const double left = remaining();
        if (left <= 0.0) { out.detail = outOfTime; return out; }
        simplexOptions.timeLimitSeconds = left;
    } else {
        simplexOptions.timeLimitSeconds = 0.0;
    }
    milp::DualSimplexResult vertex;
    try {
        vertex = milp::DualSimplexSolver{}.solveFromBasis(model, state, simplexOptions);
    } catch (const std::exception& error) {
        out.detail = std::string("not applied: simplex cleanup refused the basis: ") + error.what();
        return out;
    }
    out.pivots = vertex.iterations;
    if (vertex.status != milp::DualSimplexStatus::Optimal) {
        const char* reached = "a non-optimal status";
        switch (vertex.status) {
            case milp::DualSimplexStatus::Infeasible: reached = "Infeasible"; break;
            case milp::DualSimplexStatus::Unbounded: reached = "Unbounded"; break;
            case milp::DualSimplexStatus::IterationLimit: reached = "IterationLimit"; break;
            case milp::DualSimplexStatus::TimeLimit: reached = "TimeLimit"; break;
            default: break;
        }
        out.detail = std::string("not applied: simplex cleanup ended ") + reached + " after " +
                     std::to_string(out.pivots) + " pivots (" + std::to_string(out.basicStructural) +
                     " structural + " + std::to_string(out.basicLogical) + " logical basic)";
        return out;
    }
    if (vertex.primal.size() != static_cast<std::size_t>(n) || vertex.dual.size() != static_cast<std::size_t>(m)) {
        out.detail = "not applied: simplex cleanup returned an incomplete solution";
        return out;
    }
    // The acceptance test. The warm start assumes a dual feasible basis and
    // does not re-verify it; from a misidentified basis it could stop at a
    // primal feasible vertex that is not optimal. That vertex's objective would
    // disagree with the interior optimum, so agreement is required.
    const double disagreement = std::abs(vertex.objectiveValue - interiorObjective);
    if (!(disagreement <= 1e-6 * (1.0 + std::abs(interiorObjective)))) {
        out.detail = "not applied: vertex objective " + std::to_string(vertex.objectiveValue) +
                     " disagrees with the interior optimum " + std::to_string(interiorObjective);
        return out;
    }
    out.applied = true;
    out.primal = std::move(vertex.primal);
    out.duals = std::move(vertex.dual);
    out.objective = vertex.objectiveValue;
    out.detail = "crossover to an optimal vertex in " + std::to_string(out.pivots) + " simplex pivots";
    return out;
}

}  // namespace solver
