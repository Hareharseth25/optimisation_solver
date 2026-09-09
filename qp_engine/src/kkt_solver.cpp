#include "qp/kkt_solver.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace qp {

double KktSolver::denseDensityThreshold = 0.90;
double KktSolver::denseConstraintDensity = 0.50;
KktSolver::PathOverride KktSolver::pathOverride = KktSolver::PathOverride::Auto;

namespace {

// True density of the lower triangle of K = P + rho*A^T*A, from the symbolic
// pattern rather than an upper bound. A bound that counts each row of A's outer
// product without deduplicating overshoots badly -- it read 3.2 on a matrix that
// was 25% dense -- and would send sparse problems down the dense path.
//
// This costs one extra symbolic pass when the dense path wins. That pass is far
// cheaper than the O(n^3) factorisation it is choosing between, and on the
// sparse path the work would be done anyway.
double patternDensity(const std::vector<std::vector<Index>>& pattern, int n) {
    if (n <= 0) {
        return 1.0;
    }
    std::size_t entries = 0;
    for (const auto& column : pattern) {
        entries += column.size();
    }
    const double triangle = 0.5 * static_cast<double>(n) * (static_cast<double>(n) + 1.0);
    return static_cast<double>(entries) / triangle;
}

}  // namespace
namespace {

// Dense Cholesky factor of K (column-major, both triangles stored).
// K is overwritten with L (lower triangle).
bool denseCholesky(std::vector<double>& K, int n) {
    for (int j = 0; j < n; ++j) {
        double sum = 0.0;
        for (int k = 0; k < j; ++k) {
            const double ljk = K[static_cast<std::size_t>(j) +
                                 static_cast<std::size_t>(k) * static_cast<std::size_t>(n)];
            sum += ljk * ljk;
        }
        double diag = K[static_cast<std::size_t>(j) +
                        static_cast<std::size_t>(j) * static_cast<std::size_t>(n)] - sum;
        if (diag <= 0.0) return false;
        const double sqrtD = std::sqrt(diag);
        K[static_cast<std::size_t>(j) +
           static_cast<std::size_t>(j) * static_cast<std::size_t>(n)] = sqrtD;

        for (int i = j + 1; i < n; ++i) {
            double s = 0.0;
            for (int k = 0; k < j; ++k) {
                s += K[static_cast<std::size_t>(i) +
                       static_cast<std::size_t>(k) * static_cast<std::size_t>(n)] *
                     K[static_cast<std::size_t>(j) +
                       static_cast<std::size_t>(k) * static_cast<std::size_t>(n)];
            }
            const double kij = K[static_cast<std::size_t>(i) +
                                 static_cast<std::size_t>(j) * static_cast<std::size_t>(n)];
            K[static_cast<std::size_t>(i) +
               static_cast<std::size_t>(j) * static_cast<std::size_t>(n)] = (kij - s) / sqrtD;
        }
    }
    // Zero the upper triangle (lower triangle now holds L).
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
            K[static_cast<std::size_t>(i) +
               static_cast<std::size_t>(j) * static_cast<std::size_t>(n)] = 0.0;
    return true;
}

void denseForwardSolve(const std::vector<double>& L, int n, const double* b, double* y) {
    for (int i = 0; i < n; ++i) {
        double s = 0.0;
        for (int k = 0; k < i; ++k)
            s += L[static_cast<std::size_t>(i) +
                   static_cast<std::size_t>(k) * static_cast<std::size_t>(n)] * y[k];
        const double lii = L[static_cast<std::size_t>(i) +
                             static_cast<std::size_t>(i) * static_cast<std::size_t>(n)];
        y[i] = (std::abs(lii) < KktSolver::kSingularityEpsilon)
                   ? 0.0 : (b[i] - s) / lii;
    }
}

void denseBackSolve(const std::vector<double>& L, int n, const double* y, double* x) {
    for (int i = n - 1; i >= 0; --i) {
        double s = 0.0;
        for (int k = i + 1; k < n; ++k)
            s += L[static_cast<std::size_t>(k) +
                   static_cast<std::size_t>(i) * static_cast<std::size_t>(n)] * x[k];
        const double lii = L[static_cast<std::size_t>(i) +
                             static_cast<std::size_t>(i) * static_cast<std::size_t>(n)];
        x[i] = (std::abs(lii) < KktSolver::kSingularityEpsilon)
                   ? 0.0 : (y[i] - s) / lii;
    }
}

// Sparse utilities.
std::vector<std::vector<Index>> buildKktPattern(const QpModel& model) {
    const int n = model.numVariables();
    std::vector<std::vector<Index>> pat(static_cast<std::size_t>(n));

    // Seed the diagonal. K = P + sigma*I + rho*A^T*A always has one, and the
    // Cholesky requires it in the pattern, but neither P nor A^T*A is obliged to
    // supply it: an LP has P = 0, and a variable absent from every constraint
    // contributes nothing to A^T*A either. Until sigma was introduced the
    // diagonal only ever appeared because the adapter folded an epsilon onto
    // every entry of P's diagonal, so this seed was load-bearing and invisible.
    for (int j = 0; j < n; ++j) {
        pat[static_cast<std::size_t>(j)].push_back(static_cast<Index>(j));
    }

    // P: lower triangle (rows >= cols).
    {
        const auto& pStart = model.P.csrRowStart();
        const auto& pCols  = model.P.csrColumnIndex();
        for (int r = 0; r < n; ++r) {
            const auto begin = static_cast<std::size_t>(pStart[static_cast<std::size_t>(r)]);
            const auto end   = static_cast<std::size_t>(pStart[static_cast<std::size_t>(r) + 1]);
            for (std::size_t k = begin; k < end; ++k) {
                const Index c = pCols[k];
                if (r >= c) pat[static_cast<std::size_t>(c)].push_back(r);
            }
        }
    }

    // A^T * A: for each constraint row, for each pair of nonzeros.
    if (model.hasConstraints()) {
        const auto& aStart = model.A.csrRowStart();
        const auto& aCols  = model.A.csrColumnIndex();
        const int m = model.numConstraints();
        for (int r = 0; r < m; ++r) {
            const auto begin = static_cast<std::size_t>(aStart[static_cast<std::size_t>(r)]);
            const auto end   = static_cast<std::size_t>(aStart[static_cast<std::size_t>(r) + 1]);
            for (std::size_t k1 = begin; k1 < end; ++k1) {
                for (std::size_t k2 = k1 + 1; k2 < end; ++k2) {
                    const Index c1 = aCols[k1];
                    const Index c2 = aCols[k2];
                    pat[static_cast<std::size_t>(std::min(c1, c2))]
                       .push_back(std::max(c1, c2));
                }
            }
        }
    }

    for (int j = 0; j < n; ++j) {
        auto& col = pat[static_cast<std::size_t>(j)];
        std::sort(col.begin(), col.end());
        col.erase(std::unique(col.begin(), col.end()), col.end());
    }
    return pat;
}

void buildKktNumeric(const QpModel& model, double rho,
    std::vector<std::vector<Index>>& pat,
    std::vector<std::vector<double>>& vals)
{
    const int n = model.numVariables();
    vals.assign(static_cast<std::size_t>(n), {});
    for (int j = 0; j < n; ++j)
        vals[static_cast<std::size_t>(j)].assign(pat[static_cast<std::size_t>(j)].size(), 0.0);

    // sigma * I. The diagonal is the first entry of every column's pattern by
    // construction (buildKktPattern seeds it before anything else, and the sort
    // that follows leaves the smallest row index first).
    for (int j = 0; j < n; ++j) {
        auto& col = pat[static_cast<std::size_t>(j)];
        for (std::size_t k = 0; k < col.size(); ++k) {
            if (col[k] == static_cast<Index>(j)) {
                vals[static_cast<std::size_t>(j)][k] += KktSolver::kSigma;
                break;
            }
        }
    }

    // P (lower triangle).
    {
        const auto& pStart = model.P.csrRowStart();
        const auto& pCols  = model.P.csrColumnIndex();
        const auto& pVals  = model.P.csrValues();
        for (int r = 0; r < n; ++r) {
            const auto begin = static_cast<std::size_t>(pStart[static_cast<std::size_t>(r)]);
            const auto end   = static_cast<std::size_t>(pStart[static_cast<std::size_t>(r) + 1]);
            for (std::size_t k = begin; k < end; ++k) {
                const Index c = pCols[k];
                if (r >= c) {
                    auto& p = pat[static_cast<std::size_t>(c)];
                    const auto it = std::lower_bound(p.begin(), p.end(), r);
                    if (it != p.end() && *it == r) {
                        vals[static_cast<std::size_t>(c)]
                            [static_cast<std::size_t>(std::distance(p.begin(), it))] += pVals[k];
                    }
                }
            }
        }
    }

    // rho * A^T * A.  Iterate over the upper triangle of (k1, k2) within each
    // row to avoid double-counting off-diagonal entries.
    if (model.hasConstraints()) {
        const auto& aStart = model.A.csrRowStart();
        const auto& aCols  = model.A.csrColumnIndex();
        const auto& aVals  = model.A.csrValues();
        const int m = model.numConstraints();
        for (int r = 0; r < m; ++r) {
            const auto begin = static_cast<std::size_t>(aStart[static_cast<std::size_t>(r)]);
            const auto end   = static_cast<std::size_t>(aStart[static_cast<std::size_t>(r) + 1]);
            for (std::size_t k1 = begin; k1 < end; ++k1) {
                const double a1 = aVals[k1];
                const Index c1  = aCols[k1];
                // Diagonal contribution.
                auto& pdiag = pat[static_cast<std::size_t>(c1)];
                const auto itD = std::lower_bound(pdiag.begin(), pdiag.end(), c1);
                if (itD != pdiag.end() && *itD == c1) {
                    vals[static_cast<std::size_t>(c1)]
                        [static_cast<std::size_t>(std::distance(pdiag.begin(), itD))] += rho * a1 * a1;
                }
                for (std::size_t k2 = k1 + 1; k2 < end; ++k2) {
                    const double a2 = aVals[k2];
                    const Index c2  = aCols[k2];
                    const Index lo = std::min(c1, c2);
                    const Index hi = std::max(c1, c2);
                    auto& p = pat[static_cast<std::size_t>(lo)];
                    const auto it = std::lower_bound(p.begin(), p.end(), hi);
                    if (it != p.end() && *it == hi) {
                        vals[static_cast<std::size_t>(lo)]
                            [static_cast<std::size_t>(std::distance(p.begin(), it))] += rho * a1 * a2;
                    }
                }
            }
        }
    }
}

// Left-looking sparse Cholesky, K = L L^T, column storage.
//
// For column j: scatter K(:,j) over rows >= j, subtract the contribution of every
// earlier column k that has a nonzero at row j, then normalise.
//
//     w <- K(:,j)
//     for each k < j with L(j,k) != 0:
//         for each r >= j in column k:  w(r) -= L(r,k) * L(j,k)
//     L(j,j) = sqrt(w(j));  L(r,j) = w(r) / L(j,j) for r > j
//
// The previous implementation located L(j,k) and then never read its value,
// multiplying by the workspace entry w(k) instead and updating rows r > k rather
// than r >= j. That is not a Cholesky, and because kDenseThreshold routed every
// problem with n <= 500 to the dense path, no test ever executed it.
bool sparseCholeskyFactor(
    const std::vector<std::vector<Index>>& kPat,
    const std::vector<std::vector<double>>& kVal,
    std::vector<std::vector<Index>>& lPat,
    std::vector<std::vector<double>>& lVal,
    std::vector<double>& lDiag,
    std::vector<double>& work,
    std::vector<Index>& touched,
    std::vector<char>& marked)
{
    const int n = static_cast<int>(kPat.size());
    lPat.assign(static_cast<std::size_t>(n), {});
    lVal.assign(static_cast<std::size_t>(n), {});
    lDiag.assign(static_cast<std::size_t>(n), 0.0);
    work.assign(static_cast<std::size_t>(n), 0.0);
    marked.assign(static_cast<std::size_t>(n), 0);

    for (int j = 0; j < n; ++j) {
        touched.clear();

        const auto& kp = kPat[static_cast<std::size_t>(j)];
        const auto& kv = kVal[static_cast<std::size_t>(j)];
        for (std::size_t t = 0; t < kp.size(); ++t) {
            const Index r = kp[t];
            if (r < j) {
                continue;   // upper triangle is implied by symmetry
            }
            if (!marked[static_cast<std::size_t>(r)]) {
                marked[static_cast<std::size_t>(r)] = 1;
                touched.push_back(r);
            }
            work[static_cast<std::size_t>(r)] += kv[t];
        }

        for (int k = 0; k < j; ++k) {
            const auto& pk = lPat[static_cast<std::size_t>(k)];
            const auto& vk = lVal[static_cast<std::size_t>(k)];
            const auto it = std::lower_bound(pk.begin(), pk.end(), j);
            if (it == pk.end() || *it != j) {
                continue;
            }
            const double ljk = vk[static_cast<std::size_t>(it - pk.begin())];
            if (ljk == 0.0) {
                continue;
            }
            for (std::size_t t = 0; t < pk.size(); ++t) {
                const Index r = pk[t];
                if (r < j) {
                    continue;
                }
                if (!marked[static_cast<std::size_t>(r)]) {
                    marked[static_cast<std::size_t>(r)] = 1;
                    touched.push_back(r);
                }
                work[static_cast<std::size_t>(r)] -= vk[t] * ljk;
            }
        }

        const double d = work[static_cast<std::size_t>(j)];
        if (!(d > KktSolver::kSingularityEpsilon)) {
            for (const Index r : touched) {
                work[static_cast<std::size_t>(r)] = 0.0;
                marked[static_cast<std::size_t>(r)] = 0;
            }
            return false;
        }
        const double s = std::sqrt(d);
        lDiag[static_cast<std::size_t>(j)] = s;

        std::sort(touched.begin(), touched.end());
        auto& pj = lPat[static_cast<std::size_t>(j)];
        auto& vj = lVal[static_cast<std::size_t>(j)];
        pj.reserve(touched.size());
        vj.reserve(touched.size());
        for (const Index r : touched) {
            if (r > j && work[static_cast<std::size_t>(r)] != 0.0) {
                pj.push_back(r);
                vj.push_back(work[static_cast<std::size_t>(r)] / s);
            }
            work[static_cast<std::size_t>(r)] = 0.0;
            marked[static_cast<std::size_t>(r)] = 0;
        }
    }
    return true;
}

// Solve L y = b, column oriented: once y(j) is known it is pushed into the rows
// below it. Row-oriented forward substitution would need L by rows, which this
// storage does not provide -- the previous version searched column j for entries
// above the diagonal, found none, and silently reduced to y(j) = b(j)/diag(j).
void sparseForwardSolve(const std::vector<std::vector<Index>>& lPat,
                        const std::vector<std::vector<double>>& lVal,
                        const std::vector<double>& lDiag,
                        const double* b, double* y) {
    const int n = static_cast<int>(lPat.size());
    for (int i = 0; i < n; ++i) {
        y[i] = b[i];
    }
    for (int j = 0; j < n; ++j) {
        const double d = lDiag[static_cast<std::size_t>(j)];
        y[j] = (std::abs(d) < KktSolver::kSingularityEpsilon) ? 0.0 : y[j] / d;
        const auto& pj = lPat[static_cast<std::size_t>(j)];
        const auto& vj = lVal[static_cast<std::size_t>(j)];
        const double yj = y[j];
        for (std::size_t t = 0; t < pj.size(); ++t) {
            y[pj[t]] -= vj[t] * yj;
        }
    }
}

// Solve L^T x = y. Row i of L^T is column i of L, so this reads the same storage
// backwards.
void sparseBackSolve(const std::vector<std::vector<Index>>& lPat,
                     const std::vector<std::vector<double>>& lVal,
                     const std::vector<double>& lDiag,
                     const double* y, double* x) {
    const int n = static_cast<int>(lPat.size());
    for (int i = 0; i < n; ++i) {
        x[i] = y[i];
    }
    for (int j = n - 1; j >= 0; --j) {
        const auto& pj = lPat[static_cast<std::size_t>(j)];
        const auto& vj = lVal[static_cast<std::size_t>(j)];
        double sum = 0.0;
        for (std::size_t t = 0; t < pj.size(); ++t) {
            sum += vj[t] * x[pj[t]];
        }
        const double d = lDiag[static_cast<std::size_t>(j)];
        x[j] = (std::abs(d) < KktSolver::kSingularityEpsilon)
                   ? 0.0 : (x[j] - sum) / d;
    }
}

}  // namespace

// =============================================================================
// DenseImpl
// =============================================================================

KktSolver::DenseImpl::DenseImpl(const QpModel& model, int n)
    : model_(&model), n_(n) {
    K_.assign(static_cast<std::size_t>(n_) * static_cast<std::size_t>(n_), 0.0);
}

void KktSolver::DenseImpl::buildDenseK(double rho) {
    const int n = n_;
    std::fill(K_.begin(), K_.end(), 0.0);

    // sigma * I -- see KktSolver::kSigma. Keeps K definite when P is singular,
    // and cancels against the +sigma*x_prev the x-update puts on the RHS.
    for (int j = 0; j < n; ++j) {
        K_[static_cast<std::size_t>(j) + static_cast<std::size_t>(j) * static_cast<std::size_t>(n)] +=
            KktSolver::kSigma;
    }

    const auto& pStart = model_->P.csrRowStart();
    const auto& pCols  = model_->P.csrColumnIndex();
    const auto& pVals  = model_->P.csrValues();

    // P is stored as the FULL symmetric Hessian: both (i,j) and (j,i) are
    // present. Copy entries as they are.
    //
    // Mirroring off-diagonals here -- as an earlier version did -- doubled every
    // one of them, because the transpose entry is already in the matrix. For
    // P = [[2,1],[1,2]] that produced K = [[2,2],[2,2]], which is singular, so
    // Cholesky failed on any Hessian with a coupling term. Only diagonal
    // Hessians survived, which is why the unit tests did not catch it.
    //
    // Full symmetric storage is not negotiable: admm_solver computes P*x with a
    // plain CSR product, which is wrong for a triangular matrix, and the sparse
    // KKT path already reads the lower triangle of a full matrix.
    for (int r = 0; r < n; ++r) {
        const auto begin = static_cast<std::size_t>(pStart[static_cast<std::size_t>(r)]);
        const auto end   = static_cast<std::size_t>(pStart[static_cast<std::size_t>(r) + 1]);
        for (std::size_t k = begin; k < end; ++k) {
            const Index c = pCols[k];
            K_[static_cast<std::size_t>(r) + static_cast<std::size_t>(c) * static_cast<std::size_t>(n)] += pVals[k];
        }
    }

    if (model_->hasConstraints()) {
        const auto& aStart = model_->A.csrRowStart();
        const auto& aCols  = model_->A.csrColumnIndex();
        const auto& aVals  = model_->A.csrValues();
        const int m = model_->numConstraints();
        for (int r = 0; r < m; ++r) {
            const auto begin = static_cast<std::size_t>(aStart[static_cast<std::size_t>(r)]);
            const auto end   = static_cast<std::size_t>(aStart[static_cast<std::size_t>(r) + 1]);
            for (std::size_t k1 = begin; k1 < end; ++k1) {
                const double a1 = aVals[k1];
                const Index c1  = aCols[k1];
                // Diagonal entry: A^T*A[c1,c1] += rho * a1 * a1
                K_[static_cast<std::size_t>(c1) + static_cast<std::size_t>(c1) * static_cast<std::size_t>(n)] += rho * a1 * a1;
                for (std::size_t k2 = k1 + 1; k2 < end; ++k2) {
                    const double a2 = aVals[k2];
                    const Index c2  = aCols[k2];
                    const double v = rho * a1 * a2;
                    // K[c1,c2] and K[c2,c1] both get v (symmetric).
                    K_[static_cast<std::size_t>(c1) + static_cast<std::size_t>(c2) * static_cast<std::size_t>(n)] += v;
                    K_[static_cast<std::size_t>(c2) + static_cast<std::size_t>(c1) * static_cast<std::size_t>(n)] += v;
                }
            }
        }
    }
}

bool KktSolver::DenseImpl::factor(double rho) {
    buildDenseK(rho);
    return denseCholesky(K_, n_);
}

bool KktSolver::DenseImpl::solve(std::vector<double>& rhs) const {
    std::vector<double> y(static_cast<std::size_t>(n_));
    denseForwardSolve(K_, n_, rhs.data(), y.data());
    denseBackSolve(K_, n_, y.data(), rhs.data());
    return true;
}

// =============================================================================
// SparseImpl
// =============================================================================

KktSolver::SparseImpl::SparseImpl(const QpModel& model, int n)
    : model_(&model), n_(n), kPattern_(buildKktPattern(model)) {
    lDiag_.assign(static_cast<std::size_t>(n_), 0.0);
    work_.assign(static_cast<std::size_t>(n_), 0.0);
    marked_.assign(static_cast<std::size_t>(n_), 0);
}

bool KktSolver::SparseImpl::factor(double rho) {
    // Refill K's numbers against K's own pattern, then factor into separate
    // storage so the pattern survives for the next rho.
    buildKktNumeric(*model_, rho, kPattern_, kValues_);
    return sparseCholeskyFactor(kPattern_, kValues_, lPattern_, lValues_, lDiag_,
                                work_, touched_, marked_);
}

bool KktSolver::SparseImpl::solve(std::vector<double>& rhs) const {
    if (n_ == 0) {
        return true;
    }
    std::vector<double> y(static_cast<std::size_t>(n_));
    sparseForwardSolve(lPattern_, lValues_, lDiag_, rhs.data(), y.data());
    sparseBackSolve(lPattern_, lValues_, lDiag_, y.data(), rhs.data());
    return true;
}

// =============================================================================
// KktSolver
// =============================================================================

KktSolver::KktSolver(const QpModel& model, double rho) {
    n_ = model.numVariables();
    m_ = model.numConstraints();
    if (n_ < 0) throw std::invalid_argument("KktSolver: negative n");
    if (rho <= 0.0) throw std::invalid_argument("KktSolver: rho must be positive");
    if (n_ == 0) { factorValid_ = true; return; }

    model_ = model;
    currentRho_ = rho;
    switch (KktSolver::pathOverride) {
        case PathOverride::ForceDense:  useDense_ = true;  break;
        case PathOverride::ForceSparse: useDense_ = false; break;
        case PathOverride::Auto: {
            // Two conditions, because K's density alone does not predict the
            // winner: measured at n = 400 and n = 800 the KKT pattern was 100%
            // dense and the SPARSE path still won by roughly 2x. What separates
            // the cases is whether A itself is dense. A sparse A keeps the
            // per-iteration products cheap, and those dominate once the
            // factorisation is amortised over the iterations.
            density_ = patternDensity(buildKktPattern(model_), n_);
            const double aCells = static_cast<double>(model_.A.rows()) *
                                  static_cast<double>(n_);
            const double aDensity = aCells > 0.0
                ? static_cast<double>(model_.A.nonzeros()) / aCells
                : 1.0;
            useDense_ = density_ >= KktSolver::denseDensityThreshold &&
                        aDensity >= KktSolver::denseConstraintDensity;
            break;
        }
    }

    if (useDense_)
        denseImpl_ = std::make_unique<DenseImpl>(model_, n_);
    else
        sparseImpl_ = std::make_unique<SparseImpl>(model_, n_);

    buildKkt(rho);
    factorValid_ = numericFactor();
}

void KktSolver::buildKkt(double /*rho*/) {
    // Pattern is built in the impl constructor; nothing extra needed.
}

bool KktSolver::numericFactor() {
    if (n_ == 0) return true;
    if (useDense_) return denseImpl_->factor(currentRho_);
    return sparseImpl_->factor(currentRho_);
}

bool KktSolver::refactor(double rho) {
    if (n_ == 0) { factorValid_ = true; return true; }
    if (rho <= 0.0) return false;
    currentRho_ = rho;
    buildKkt(rho);
    factorValid_ = numericFactor();
    return factorValid_;
}

bool KktSolver::solve(std::vector<double>& x) const {
    if (n_ == 0) return true;
    if (!factorValid_) return false;
    if (useDense_) return denseImpl_->solve(x);
    return sparseImpl_->solve(x);
}

}  // namespace qp
