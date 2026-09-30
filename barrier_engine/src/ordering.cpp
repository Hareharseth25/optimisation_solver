#include "barrier/sparse.h"

#include <algorithm>

namespace barrier {
namespace {

// Quotient-graph minimum degree.
//
// The graph is held as two kinds of adjacency per surviving variable: direct
// edges to other variables, and memberships in ELEMENTS (the cliques left
// behind by eliminated variables). That representation is what keeps the cost
// bounded -- eliminating a variable would otherwise add a dense clique to the
// graph explicitly, which on a few thousand rows exhausts memory long before
// the ordering finishes.
//
// Degrees are AMD's upper bound |A_i| + sum_e (|L_e| - 1) rather than the exact
// external degree. Computing the exact degree means unioning the element lists
// at every step, which costs more than the factorisation the ordering exists to
// cheapen. The bound over-counts variables shared between two elements, so it
// can order slightly worse than exact minimum degree; it cannot order
// INVALIDLY, because any permutation is numerically valid here -- see the
// quasidefinite note on LdlFactorization.
//
// Not implemented: supervariable (indistinguishable node) detection and
// mass elimination. Both are quality/speed refinements in AMD proper.
class MinimumDegree {
public:
    explicit MinimumDegree(const SparseCsc& upper) : n_(upper.cols) {
        const auto size = static_cast<std::size_t>(n_);
        variableAdjacency_.resize(size);
        elementAdjacency_.resize(size);
        elementVariables_.resize(size);
        for (Index j = 0; j < n_; ++j) {
            for (Index p = upper.columnStart[j]; p < upper.columnStart[j + 1]; ++p) {
                const Index i = upper.rowIndex[p];
                if (i == j) continue;  // the diagonal carries no adjacency
                variableAdjacency_[static_cast<std::size_t>(i)].push_back(j);
                variableAdjacency_[static_cast<std::size_t>(j)].push_back(i);
            }
        }
        for (auto& adjacency : variableAdjacency_) {
            std::sort(adjacency.begin(), adjacency.end());
            adjacency.erase(std::unique(adjacency.begin(), adjacency.end()), adjacency.end());
        }
        alive_.assign(size, 1);
        isElement_.assign(size, 0);
        inElement_.assign(size, 0);
        degree_.resize(size);
        bucketOf_.assign(size, -1);
        next_.assign(size, -1);
        previous_.assign(size, -1);
        bucketHead_.assign(size + 1, -1);
        for (Index i = 0; i < n_; ++i) {
            degree_[static_cast<std::size_t>(i)] =
                static_cast<Index>(variableAdjacency_[static_cast<std::size_t>(i)].size());
            link(i);
        }
    }

    void run(std::vector<Index>& permutation) {
        permutation.clear();
        permutation.reserve(static_cast<std::size_t>(n_));
        Index minimum = 0;
        for (Index step = 0; step < n_; ++step) {
            while (minimum <= n_ && bucketHead_[static_cast<std::size_t>(minimum)] < 0) ++minimum;
            if (minimum > n_) break;  // defensive; every alive variable is bucketed
            const Index pivot = bucketHead_[static_cast<std::size_t>(minimum)];
            unlink(pivot);
            alive_[static_cast<std::size_t>(pivot)] = 0;
            permutation.push_back(pivot);
            minimum = eliminate(pivot, step, minimum);
        }
    }

private:
    void link(Index i) {
        Index d = std::clamp(degree_[static_cast<std::size_t>(i)], 0, n_);
        next_[static_cast<std::size_t>(i)] = bucketHead_[static_cast<std::size_t>(d)];
        previous_[static_cast<std::size_t>(i)] = -1;
        if (bucketHead_[static_cast<std::size_t>(d)] >= 0)
            previous_[static_cast<std::size_t>(bucketHead_[static_cast<std::size_t>(d)])] = i;
        bucketHead_[static_cast<std::size_t>(d)] = i;
        bucketOf_[static_cast<std::size_t>(i)] = d;
    }

    void unlink(Index i) {
        const Index d = bucketOf_[static_cast<std::size_t>(i)];
        if (previous_[static_cast<std::size_t>(i)] >= 0)
            next_[static_cast<std::size_t>(previous_[static_cast<std::size_t>(i)])] =
                next_[static_cast<std::size_t>(i)];
        else if (d >= 0 && bucketHead_[static_cast<std::size_t>(d)] == i)
            bucketHead_[static_cast<std::size_t>(d)] = next_[static_cast<std::size_t>(i)];
        if (next_[static_cast<std::size_t>(i)] >= 0)
            previous_[static_cast<std::size_t>(next_[static_cast<std::size_t>(i)])] =
                previous_[static_cast<std::size_t>(i)];
        next_[static_cast<std::size_t>(i)] = previous_[static_cast<std::size_t>(i)] = -1;
        bucketOf_[static_cast<std::size_t>(i)] = -1;
    }

    Index eliminate(Index pivot, Index step, Index minimum) {
        // The new element: everything still alive that the pivot reached,
        // directly or through an element it belonged to.
        std::vector<Index> element;
        auto consider = [&](Index v) {
            if (v == pivot || !alive_[static_cast<std::size_t>(v)]) return;
            if (inElement_[static_cast<std::size_t>(v)]) return;
            inElement_[static_cast<std::size_t>(v)] = 1;
            element.push_back(v);
        };
        for (Index v : variableAdjacency_[static_cast<std::size_t>(pivot)]) consider(v);
        for (Index e : elementAdjacency_[static_cast<std::size_t>(pivot)]) {
            if (!isElement_[static_cast<std::size_t>(e)]) continue;
            for (Index v : elementVariables_[static_cast<std::size_t>(e)]) consider(v);
        }

        // Absorb the elements the pivot belonged to: the new element covers
        // every connection they represented.
        for (Index e : elementAdjacency_[static_cast<std::size_t>(pivot)]) {
            isElement_[static_cast<std::size_t>(e)] = 0;
            elementVariables_[static_cast<std::size_t>(e)].clear();
            elementVariables_[static_cast<std::size_t>(e)].shrink_to_fit();
        }

        elementVariables_[static_cast<std::size_t>(pivot)] = element;
        isElement_[static_cast<std::size_t>(pivot)] = 1;
        variableAdjacency_[static_cast<std::size_t>(pivot)].clear();
        variableAdjacency_[static_cast<std::size_t>(pivot)].shrink_to_fit();
        elementAdjacency_[static_cast<std::size_t>(pivot)].clear();
        elementAdjacency_[static_cast<std::size_t>(pivot)].shrink_to_fit();

        Index newMinimum = minimum;
        const Index remaining = n_ - step - 1;
        for (Index i : element) {
            auto& variables = variableAdjacency_[static_cast<std::size_t>(i)];
            // A direct edge to another member of this element is now redundant:
            // the element records that adjacency. Pruning these is what stops
            // the adjacency lists growing without bound.
            variables.erase(std::remove_if(variables.begin(), variables.end(), [&](Index v) {
                return v == i || v == pivot || !alive_[static_cast<std::size_t>(v)] ||
                       inElement_[static_cast<std::size_t>(v)] != 0;
            }), variables.end());

            auto& elements = elementAdjacency_[static_cast<std::size_t>(i)];
            elements.erase(std::remove_if(elements.begin(), elements.end(), [&](Index e) {
                return !isElement_[static_cast<std::size_t>(e)];
            }), elements.end());
            elements.push_back(pivot);

            Index bound = static_cast<Index>(variables.size());
            for (Index e : elements) {
                const auto size = static_cast<Index>(
                    elementVariables_[static_cast<std::size_t>(e)].size());
                bound += size > 0 ? size - 1 : 0;
            }
            bound = std::clamp(bound, 0, std::max(remaining, 0));

            unlink(i);
            degree_[static_cast<std::size_t>(i)] = bound;
            link(i);
            if (bound < newMinimum) newMinimum = bound;
        }
        for (Index v : element) inElement_[static_cast<std::size_t>(v)] = 0;
        return newMinimum;
    }

    Index n_;
    std::vector<std::vector<Index>> variableAdjacency_, elementAdjacency_, elementVariables_;
    std::vector<char> alive_, isElement_, inElement_;
    std::vector<Index> degree_, bucketOf_, next_, previous_, bucketHead_;
};

}  // namespace

void approximateMinimumDegree(const SparseCsc& upper, std::vector<Index>& permutation) {
    permutation.clear();
    if (upper.cols <= 0) return;
    MinimumDegree ordering(upper);
    ordering.run(permutation);
    // Defensive completion: every index must appear exactly once. A partial
    // ordering would silently drop rows from the factorisation, and the LDL'
    // would then factorise a different matrix than the one analysed.
    if (static_cast<Index>(permutation.size()) != upper.cols) {
        std::vector<char> seen(static_cast<std::size_t>(upper.cols), 0);
        for (Index v : permutation) seen[static_cast<std::size_t>(v)] = 1;
        for (Index v = 0; v < upper.cols; ++v)
            if (!seen[static_cast<std::size_t>(v)]) permutation.push_back(v);
    }
}

}  // namespace barrier
