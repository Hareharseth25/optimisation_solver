#pragma once

#include <cstdint>
#include <vector>

namespace milp {

// The status of one column in the bounded-variable simplex tableau: a
// structural variable, or the implicit logical/slack variable a constraint
// row carries (see dual_simplex_solver.cpp for the [A | -I] formulation).
enum class BasisStatus : std::uint8_t {
    Basic,
    AtLower,
    AtUpper,
    Free,
    Fixed
};

// A simplex basis, described purely by per-column status -- no row-to-column
// assignment or numeric values are stored. This is deliberate: it is the same
// minimal representation real solvers persist for warm starts (e.g. an MPS
// basis file), and it is exactly what a branch-and-cut tree node needs to
// snapshot cheaply and hand back to DualSimplexSolver::solveFromBasis after a
// bound change -- the solver reconstructs and refactorizes the basis matrix
// from the status arrays alone.
struct BasisState {
    std::vector<BasisStatus> variableStatus;   // size = number of structural variables
    std::vector<BasisStatus> constraintStatus; // size = number of constraints

    // Explicit deep copy. std::vector already deep-copies on assignment, so
    // this is provided for call-site clarity where a tree node snapshots a
    // basis -- `node.basis = parent.basis.clone();` reads as an intentional
    // copy rather than leaving the reader to know std::vector's semantics.
    [[nodiscard]] BasisState clone() const;

    // Compact in-process binary encoding (native endianness/layout -- this is
    // a snapshot format for a single solve run's tree nodes, not a portable
    // file format). Layout: [u64 variableCount][u64 constraintCount]
    // [1 byte per variable status][1 byte per constraint status].
    [[nodiscard]] std::vector<std::uint8_t> serialize() const;
    [[nodiscard]] static BasisState deserialize(const std::vector<std::uint8_t> &bytes);
};

[[nodiscard]] bool operator==(const BasisState &a, const BasisState &b);
[[nodiscard]] inline bool operator!=(const BasisState &a, const BasisState &b) {
    return !(a == b);
}

} // namespace milp
