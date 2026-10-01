#pragma once
// model::Model  <->  BarrierProblem.
//
// The model's form is   opt  c'x + sum q_ij x_i x_j + offset
//                       s.t. rl <= A x <= ru,   l <= x <= u
// and the barrier's is  min  c'x + 0.5 x'Qx   s.t.  A x = b,  l < x < u.
//
// Three transformations bridge them, and each has a way to go subtly wrong:
//
//  1. Rows. A ranged or one-sided row gets a slack: a'x - s = 0 with
//     rl <= s <= ru. An equality row (rl == ru) stays an equality with no
//     slack, because a slack with rl == ru would be a fixed variable, which
//     has no interior. A row free on both sides constrains nothing and is
//     dropped; its multiplier is zero.
//
//  2. Fixed variables (l == u) have no interior either, so they are
//     substituted out: their columns move to the right-hand side, their linear
//     and quadratic contributions fold into c and the offset.
//
//  3. Sense and curvature. A maximisation is negated into a minimisation.
//     Model quadratic terms carry the DIRECT coefficient of x_i x_j with no
//     implicit 1/2, so the Hessian of 0.5 x'Qx has Q_ii = 2 q_ii and
//     Q_ij = q_ij off the diagonal.
//
// Dual convention on the way back. For a row with a slack, stationarity in the
// slack gives y_i = zLower_s - zUpper_s, and the sensitivity of the optimum to
// whichever side is active is exactly y_i -- so y_i is the minimisation's
// shadow price for that row regardless of which side binds. Undoing the
// negation of a maximisation negates it once more. The result is
// d(objective, model's own sense)/d(row bound), the same convention the dual
// simplex reports: on "max 3x+5y s.t. x<=4, 2y<=12, 3x+2y<=18" both engines
// return [0, 1.5, 1].
#include "barrier/barrier_solver.h"
#include "model/model.h"

#include <string>
#include <vector>

namespace barrier {

struct Translation {
    bool ok = false;
    std::string error;

    bool negated = false;  // true when the model maximises

    // Model variable j -> barrier column, or -1 when j was fixed and removed.
    std::vector<Index> columnOf;
    std::vector<double> fixedValue;  // valid where columnOf[j] == -1

    // Model row i -> barrier row, or -1 when the row was free and dropped.
    std::vector<Index> rowOf;

    Index structuralColumns = 0;  // barrier columns [0, structural) are model variables
    std::size_t fixedVariables = 0, slackColumns = 0, droppedRows = 0;
};

// Builds the barrier problem. Integer variables are accepted and relaxed: the
// barrier method is continuous, and it is the DISPATCHER's job to keep integer
// models away from it unless the caller forces a relaxation.
[[nodiscard]] Translation toBarrierProblem(const model::Model& model, BarrierProblem& problem);

struct ModelSolution {
    std::vector<double> variableValues;  // length model.variables.size()
    std::vector<double> constraintDuals; // shadow prices, model's own sense
    double objectiveValue = 0.0;         // model's own sense, offset included
};

[[nodiscard]] ModelSolution toModelSolution(const model::Model& model,
                                            const Translation& translation,
                                            const Result& result);

}  // namespace barrier
