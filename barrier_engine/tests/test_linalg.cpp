// Linear algebra underneath the barrier method: CSC assembly, the fill-reducing
// ordering, and the quasidefinite LDL'.
//
// The LDL' is checked by RESIDUAL against an independently built dense solve,
// not against itself: a factorisation that is internally consistent but wrong
// would otherwise pass. The ordering is checked for fill reduction on a matrix
// whose natural order provably fills completely.
#include "barrier/sparse.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using namespace barrier;

namespace {
void check(bool ok, const std::string& what) {
    if (!ok) throw std::runtime_error(what);
}

// Dense Gaussian elimination with partial pivoting: the independent reference.
std::vector<double> denseSolve(std::vector<std::vector<double>> a, std::vector<double> b) {
    const int n = static_cast<int>(b.size());
    for (int k = 0; k < n; ++k) {
        int best = k;
        for (int i = k + 1; i < n; ++i)
            if (std::abs(a[static_cast<std::size_t>(i)][static_cast<std::size_t>(k)]) >
                std::abs(a[static_cast<std::size_t>(best)][static_cast<std::size_t>(k)])) best = i;
        std::swap(a[static_cast<std::size_t>(k)], a[static_cast<std::size_t>(best)]);
        std::swap(b[static_cast<std::size_t>(k)], b[static_cast<std::size_t>(best)]);
        const double pivot = a[static_cast<std::size_t>(k)][static_cast<std::size_t>(k)];
        check(std::abs(pivot) > 1e-14, "dense reference hit a singular pivot");
        for (int i = k + 1; i < n; ++i) {
            const double f = a[static_cast<std::size_t>(i)][static_cast<std::size_t>(k)] / pivot;
            if (f == 0.0) continue;
            for (int j = k; j < n; ++j)
                a[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] -=
                    f * a[static_cast<std::size_t>(k)][static_cast<std::size_t>(j)];
            b[static_cast<std::size_t>(i)] -= f * b[static_cast<std::size_t>(k)];
        }
    }
    std::vector<double> x(static_cast<std::size_t>(n), 0.0);
    for (int i = n - 1; i >= 0; --i) {
        double sum = b[static_cast<std::size_t>(i)];
        for (int j = i + 1; j < n; ++j)
            sum -= a[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] *
                   x[static_cast<std::size_t>(j)];
        x[static_cast<std::size_t>(i)] = sum / a[static_cast<std::size_t>(i)][static_cast<std::size_t>(i)];
    }
    return x;
}

std::vector<std::vector<double>> toDense(const SparseCsc& upper) {
    const auto n = static_cast<std::size_t>(upper.cols);
    std::vector<std::vector<double>> dense(n, std::vector<double>(n, 0.0));
    for (Index j = 0; j < upper.cols; ++j)
        for (Index p = upper.columnStart[j]; p < upper.columnStart[j + 1]; ++p) {
            const Index i = upper.rowIndex[p];
            dense[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] = upper.value[p];
            dense[static_cast<std::size_t>(j)][static_cast<std::size_t>(i)] = upper.value[p];
        }
    return dense;
}

// Factor, solve, and report the residual ||A*x - b||_inf measured on the
// ORIGINAL matrix, plus the gap against the dense reference solution.
struct Outcome { double residual, referenceGap, referenceResidual; std::size_t fill; int negativePivots; };
Outcome factorAndSolve(const SparseCsc& upper, const std::vector<double>& b, bool reorder) {
    std::vector<Index> permutation;
    if (reorder) approximateMinimumDegree(upper, permutation);
    else { permutation.resize(static_cast<std::size_t>(upper.cols)); for (Index i = 0; i < upper.cols; ++i) permutation[static_cast<std::size_t>(i)] = i; }

    const SparseCsc permuted = permuteSymmetric(upper, permutation);
    check(permuted.validate(), "permuted matrix failed structural validation");

    LdlFactorization factorization;
    check(factorization.analyse(permuted), "analyse failed");
    check(factorization.factor(permuted, 1e-13), "factor failed");

    const auto n = static_cast<std::size_t>(upper.cols);
    std::vector<Index> inverse(n);
    for (Index k = 0; k < upper.cols; ++k) inverse[static_cast<std::size_t>(permutation[static_cast<std::size_t>(k)])] = k;

    std::vector<double> rhs(n);
    for (std::size_t i = 0; i < n; ++i) rhs[static_cast<std::size_t>(inverse[i])] = b[i];
    factorization.solveRefined(permuted, rhs, 2);
    std::vector<double> x(n);
    for (std::size_t i = 0; i < n; ++i) x[i] = rhs[static_cast<std::size_t>(inverse[i])];

    const auto dense = toDense(upper);
    double residual = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        double row = 0.0;
        for (std::size_t j = 0; j < n; ++j) row += dense[i][j] * x[j];
        residual = std::max(residual, std::abs(row - b[i]));
    }
    // The reference's OWN residual is the scale-free yardstick. On a
    // deliberately ill-conditioned matrix an absolute bound says nothing; what
    // matters is whether this unpivoted sparse factorisation is as accurate as
    // dense Gaussian elimination WITH partial pivoting on the same matrix.
    const auto reference = denseSolve(dense, b);
    double gap = 0.0, referenceResidual = 0.0;
    for (std::size_t i = 0; i < n; ++i) gap = std::max(gap, std::abs(reference[i] - x[i]));
    for (std::size_t i = 0; i < n; ++i) {
        double row = 0.0;
        for (std::size_t j = 0; j < n; ++j) row += dense[i][j] * reference[j];
        referenceResidual = std::max(referenceResidual, std::abs(row - b[i]));
    }
    return {residual, gap, referenceResidual, factorization.factorNonzeros(),
            factorization.negativePivots()};
}
}  // namespace

int main() {
    try {
        // ---- CSC assembly -------------------------------------------------
        {
            TripletBuilder builder(3, 3);
            builder.add(2, 0, 1.0); builder.add(0, 0, 2.0);
            builder.add(0, 0, 3.0);  // duplicate, must be summed
            builder.add(1, 2, 4.0);
            const SparseCsc m = builder.build();
            check(m.validate(), "triplet build produced an invalid matrix");
            check(m.nonzeros() == 3, "duplicate was not summed: " + std::to_string(m.nonzeros()));
            check(m.rowIndex[0] == 0 && m.value[0] == 5.0, "duplicate sum wrong");
            check(m.rowIndex[1] == 2 && m.value[1] == 1.0, "column 0 not ascending");
            std::vector<double> y;
            m.multiply({1.0, 0.0, 1.0}, y);
            check(y.size() == 3 && std::abs(y[0] - 5.0) < 1e-15 && std::abs(y[1] - 4.0) < 1e-15,
                  "multiply wrong");
            m.transposeMultiply({1.0, 1.0, 1.0}, y);
            check(std::abs(y[0] - 6.0) < 1e-15 && std::abs(y[2] - 4.0) < 1e-15, "transposeMultiply wrong");
        }

        // ---- ordering: fill reduction on an arrow matrix -------------------
        //
        // A matrix whose FIRST row/column is dense fills in completely under
        // the natural order and not at all when that node is eliminated last.
        // This is the canonical case that separates a real ordering from a
        // permutation that merely type-checks.
        {
            const Index n = 200;
            TripletBuilder builder(n, n);
            for (Index i = 0; i < n; ++i) builder.add(i, i, 10.0);
            // Off-diagonals kept small so the matrix is diagonally dominant and
            // hence definite: an indefinite arrow would lose accuracy in an
            // UNPIVOTED LDL' for reasons that have nothing to do with ordering,
            // and this test is about fill.
            for (Index i = 1; i < n; ++i) builder.add(0, i, 0.01);  // dense first column
            const SparseCsc arrow = builder.build();

            std::vector<double> b(static_cast<std::size_t>(n), 1.0);
            const Outcome natural = factorAndSolve(arrow, b, false);
            const Outcome ordered = factorAndSolve(arrow, b, true);
            std::printf("  arrow(%d): natural fill=%zu (res=%.2e), reordered fill=%zu (res=%.2e)\n",
                        n, natural.fill, natural.residual, ordered.fill, ordered.residual);
            check(natural.fill > 15000, "natural order should fill the arrow completely");
            check(ordered.fill == static_cast<std::size_t>(n - 1),
                  "minimum degree should leave the arrow fill-free, got " + std::to_string(ordered.fill));
            check(natural.residual < 1e-10 && ordered.residual < 1e-10, "arrow residual");
        }

        // ---- ordering: permutation validity on random patterns ------------
        {
            std::mt19937 rng(12345);
            for (int trial = 0; trial < 20; ++trial) {
                const Index n = 60;
                TripletBuilder builder(n, n);
                for (Index i = 0; i < n; ++i) builder.add(i, i, 1.0);
                std::uniform_int_distribution<Index> pick(0, n - 1);
                for (int e = 0; e < 200; ++e) {
                    Index i = pick(rng), j = pick(rng);
                    if (i > j) std::swap(i, j);
                    if (i != j) builder.add(i, j, 1.0);
                }
                std::vector<Index> permutation;
                approximateMinimumDegree(builder.build(), permutation);
                check(static_cast<Index>(permutation.size()) == n, "permutation size");
                std::vector<char> seen(static_cast<std::size_t>(n), 0);
                for (Index v : permutation) {
                    check(v >= 0 && v < n, "permutation out of range");
                    check(!seen[static_cast<std::size_t>(v)], "permutation repeated an index");
                    seen[static_cast<std::size_t>(v)] = 1;
                }
            }
        }

        // ---- LDL' on symmetric positive definite --------------------------
        {
            std::mt19937 rng(999);
            std::uniform_real_distribution<double> uniform(-1.0, 1.0);
            const Index n = 80;
            TripletBuilder builder(n, n);
            for (Index i = 0; i < n; ++i) builder.add(i, i, 20.0);
            for (Index i = 0; i < n; ++i)
                for (Index j = i + 1; j < n; ++j)
                    if (uniform(rng) > 0.75) builder.add(i, j, uniform(rng));
            const SparseCsc spd = builder.build();
            std::vector<double> b(static_cast<std::size_t>(n));
            for (auto& v : b) v = uniform(rng);
            const Outcome out = factorAndSolve(spd, b, true);
            std::printf("  spd(%d): residual=%.3e gap=%.3e negativePivots=%d\n",
                        n, out.residual, out.referenceGap, out.negativePivots);
            check(out.residual < 1e-10, "SPD residual " + std::to_string(out.residual));
            check(out.referenceGap < 1e-8, "SPD disagrees with the dense reference");
            check(out.negativePivots == 0, "SPD must have no negative pivots");
        }

        // ---- LDL' on a QUASIDEFINITE augmented system ----------------------
        //
        // [ -(D+Rp)   A' ]  with D, Rp, Rd > 0. This is the matrix the barrier
        // [    A      Rd ]  method actually factorises, and its inertia is
        // known in advance: exactly n negative pivots and m positive ones.
        // Checking that is how a sign error in the assembly gets caught.
        {
            std::mt19937 rng(4242);
            std::uniform_real_distribution<double> uniform(-1.0, 1.0);
            const Index n = 70, m = 40, total = n + m;
            TripletBuilder builder(total, total);
            for (Index j = 0; j < n; ++j) builder.add(j, j, -(1.0 + std::abs(uniform(rng))));
            for (Index i = 0; i < m; ++i) builder.add(n + i, n + i, 1e-8);
            int nonzeros = 0;
            for (Index i = 0; i < m; ++i)
                for (Index j = 0; j < n; ++j)
                    if (uniform(rng) > 0.8) { builder.add(j, n + i, uniform(rng)); ++nonzeros; }
            const SparseCsc augmented = builder.build();
            std::vector<double> b(static_cast<std::size_t>(total));
            for (auto& v : b) v = uniform(rng);
            const Outcome out = factorAndSolve(augmented, b, true);
            std::printf("  quasidefinite(%d+%d, %d offdiag): residual=%.3e (dense ref %.3e)"
                        " gap=%.3e neg=%d fill=%zu\n",
                        n, m, nonzeros, out.residual, out.referenceResidual,
                        out.referenceGap, out.negativePivots, out.fill);
            // Rd = 1e-8 makes this matrix condition ~1e8 on purpose, so the
            // bound is relative to what pivoted dense elimination achieves.
            check(out.residual <= std::max(1e-9, 100.0 * out.referenceResidual),
                  "quasidefinite residual " + std::to_string(out.residual) +
                  " far exceeds the dense reference's " + std::to_string(out.referenceResidual));
            check(out.referenceGap < 1e-6, "quasidefinite disagrees with the dense reference");
            check(out.negativePivots == n,
                  "expected " + std::to_string(n) + " negative pivots, got " +
                  std::to_string(out.negativePivots));
        }

        // ---- degenerate shapes --------------------------------------------
        {
            TripletBuilder empty(0, 0);
            const SparseCsc zero = empty.build();
            LdlFactorization factorization;
            check(factorization.analyse(zero), "analyse of an empty matrix");
            check(factorization.factor(zero, 1e-13), "factor of an empty matrix");
            std::vector<Index> permutation;
            approximateMinimumDegree(zero, permutation);
            check(permutation.empty(), "empty ordering");

            TripletBuilder one(1, 1);
            one.add(0, 0, -4.0);
            const SparseCsc single = one.build();
            const Outcome out = factorAndSolve(single, {2.0}, true);
            check(out.residual < 1e-14 && out.negativePivots == 1, "1x1 negative pivot");
        }

        std::printf("barrier linear algebra tests passed\n");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAILED: %s\n", error.what());
        return 1;
    }
}
