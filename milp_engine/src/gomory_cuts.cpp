#include "milp/gomory_cuts.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace milp {
namespace {

// A dense copy of the structural constraint matrix A (columns[j][i] =
// coefficient of variable j in row i), independent of DualSimplexSolver's
// own internal Problem -- this is the small, self-contained amount of
// column-building logic a cut generator needs; not worth sharing an internal
// header over for ~15 lines.
std::vector<std::vector<double>> buildDenseColumns(const model::Model &model) {
    const std::size_t n = model.variables.size();
    const std::size_t m = model.constraints.size();

    std::vector<std::vector<double>> columns(n, std::vector<double>(m, 0.0));
    for (std::size_t i = 0; i < m; ++i) {
        for (const auto &term : model.constraints[i].linearTerms) {
            columns[static_cast<std::size_t>(term.variableIndex)][i] += term.value;
        }
    }
    return columns;
}

// rho . column_j over the unified [structural | logical] column space,
// mirroring DualSimplexSolver's own Problem::dotColumn.
double dotColumn(
    const std::vector<double> &rho,
    int j,
    int n,
    const std::vector<std::vector<double>> &columns)
{
    if (j >= n) {
        return -rho[static_cast<std::size_t>(j - n)];
    }
    const auto &column = columns[static_cast<std::size_t>(j)];
    double sum = 0.0;
    for (std::size_t k = 0; k < rho.size(); ++k) {
        sum += rho[k] * column[k];
    }
    return sum;
}

} // namespace

std::vector<Cut> GomoryCutGenerator::generate(
    const model::Model &model,
    const DualSimplexResult &solved,
    const GomoryCutOptions &options) const
{
    std::vector<Cut> cuts;

    if (solved.status != DualSimplexStatus::Optimal) {
        return cuts;
    }

    const SimplexTableau &tableau = solved.tableau;
    const int n = tableau.numStructural;
    const int m = tableau.numRows;

    if (n != static_cast<int>(model.variables.size()) ||
        m != static_cast<int>(model.constraints.size())) {
        return cuts; // tableau doesn't match this model; nothing safe to derive.
    }

    const std::vector<std::vector<double>> columns = buildDenseColumns(model);

    for (int r = 0; r < m; ++r) {
        const int basicColumn = tableau.basisColumns[static_cast<std::size_t>(r)];
        if (basicColumn >= n) {
            continue; // basic variable is a logical/row slack, not structural.
        }
        if (model.variables[static_cast<std::size_t>(basicColumn)].type ==
            model::VariableType::Continuous) {
            continue;
        }

        const double value = solved.primal[static_cast<std::size_t>(basicColumn)];
        const double f0 = value - std::floor(value);

        // "Near-zero pivot element" guard: f0 (and 1-f0) sit in the GMI
        // formula's denominators below, so a row whose basic value is
        // almost exactly integral is skipped rather than risking a blown-up
        // coefficient from dividing by a near-zero fractional part.
        if (std::min(f0, 1.0 - f0) <= options.integralityTolerance) {
            continue;
        }

        const std::vector<double> &rho = tableau.basisInverse[static_cast<std::size_t>(r)];

        std::vector<double> structuralCoeff(static_cast<std::size_t>(n), 0.0);
        double cutRhs = 1.0;
        bool abortRow = false;

        for (int j = 0; j < n + m; ++j) {
            const BasisStatus st = tableau.columnStatus[static_cast<std::size_t>(j)];
            if (st == BasisStatus::Basic || st == BasisStatus::Fixed) {
                continue;
            }
            if (st == BasisStatus::Free) {
                // The GMI derivation assumes every nonbasic column sits at a
                // finite bound; a genuinely free nonbasic column breaks that
                // premise, so this row's cut is abandoned entirely rather
                // than silently dropping just that column's contribution
                // (which would produce an invalid inequality, not merely a
                // weaker one).
                abortRow = true;
                break;
            }

            const double alpha = dotColumn(rho, j, n, columns);

            const bool isAtUpper = (st == BasisStatus::AtUpper);
            const double beta = isAtUpper ? -alpha : alpha;

            const bool isIntegerVar = (j < n) &&
                (model.variables[static_cast<std::size_t>(j)].type != model::VariableType::Continuous);
            // Logical (row/slack) columns -- j >= n -- are always treated as
            // continuous: their coefficients aren't guaranteed integer even
            // when every structural coefficient in that row is.

            double cutCoeff;
            if (isIntegerVar) {
                const double fj = beta - std::floor(beta);
                cutCoeff = (fj <= f0) ? (fj / f0) : ((1.0 - fj) / (1.0 - f0));
            } else {
                cutCoeff = (beta >= 0.0) ? (beta / f0) : (-beta / (1.0 - f0));
            }

            const double finalCoeff = isAtUpper ? -cutCoeff : cutCoeff;
            const double boundValue = isAtUpper
                ? tableau.columnUpper[static_cast<std::size_t>(j)]
                : tableau.columnLower[static_cast<std::size_t>(j)];
            cutRhs += isAtUpper ? -(cutCoeff * boundValue) : (cutCoeff * boundValue);

            if (j < n) {
                structuralCoeff[static_cast<std::size_t>(j)] += finalCoeff;
            } else {
                // Project this logical column's contribution back onto the
                // structural variables via its row's own coefficients
                // (y_i = sum_k A[i][k] x_k), since a Cut is only ever
                // expressed over the original model's structural variables.
                const auto &rowTerms = model.constraints[static_cast<std::size_t>(j - n)].linearTerms;
                for (const auto &term : rowTerms) {
                    structuralCoeff[static_cast<std::size_t>(term.variableIndex)] +=
                        finalCoeff * term.value;
                }
            }
        }

        if (abortRow) {
            continue;
        }

        double lhsAtCurrent = 0.0;
        for (int k = 0; k < n; ++k) {
            lhsAtCurrent +=
                structuralCoeff[static_cast<std::size_t>(k)] * solved.primal[static_cast<std::size_t>(k)];
        }
        const double violation = cutRhs - lhsAtCurrent;
        if (violation < options.minimumViolation) {
            continue;
        }

        double maxAbs = 0.0;
        double minAbsNonzero = std::numeric_limits<double>::infinity();
        for (double c : structuralCoeff) {
            const double a = std::abs(c);
            if (a == 0.0) {
                continue;
            }
            maxAbs = std::max(maxAbs, a);
            minAbsNonzero = std::min(minAbsNonzero, a);
        }
        if (maxAbs == 0.0) {
            continue; // degenerate all-zero cut.
        }
        if (maxAbs / minAbsNonzero > options.maxCoefficientDynamism) {
            continue;
        }

        Cut cut;
        cut.coeffs = std::move(structuralCoeff);
        cut.rhs = cutRhs;
        cuts.push_back(std::move(cut));
    }

    return cuts;
}

} // namespace milp
