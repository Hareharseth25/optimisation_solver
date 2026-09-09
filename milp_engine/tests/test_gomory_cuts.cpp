#include "milp/branch_and_bound.h"
#include "milp/dual_simplex_solver.h"
#include "milp/gomory_cuts.h"
#include "model/model.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
#include <string>

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kTolerance = 1e-6;

bool approx(double a, double b) { return std::abs(a - b) <= kTolerance; }

model::Variable makeIntegerVariable(const std::string &name, double lower, double upper) {
  model::Variable v;
  v.name = name;
  v.type = model::VariableType::Integer;
  v.lowerBound = lower;
  v.upperBound = upper;
  return v;
}

// minimize -3x - 4y
// subject to 2x + y <= 5
//            x + 3y <= 6
//            x, y integer, >= 0
//
// (The same fractional example used by test_branch_and_bound.cpp, just
// sign-flipped into minimize form: minimizing -3x-4y and maximizing 3x+4y
// share the same optimal (x,y).) LP relaxation optimum is exactly
// x=1.8, y=1.4, objective=-11.0 (both rows binding, both integer variables
// basic and fractional). Hand-enumeration of the small integer feasible
// region gives the true optimum at x=2, y=1, objective=-10.0.
model::Model makeFractionalModel() {
  model::Model model;
  model.name = "gomory_fixture";

  model.variables.push_back(makeIntegerVariable("x", 0.0, 10.0));
  model.variables.push_back(makeIntegerVariable("y", 0.0, 10.0));

  model.objective.linearTerms.push_back({0, -3.0});
  model.objective.linearTerms.push_back({1, -4.0});

  model::Constraint c1;
  c1.name = "c1";
  c1.lowerBound = -kInf;
  c1.upperBound = 5.0;
  c1.linearTerms.push_back({0, 2.0});
  c1.linearTerms.push_back({1, 1.0});
  model.constraints.push_back(c1);

  model::Constraint c2;
  c2.name = "c2";
  c2.lowerBound = -kInf;
  c2.upperBound = 6.0;
  c2.linearTerms.push_back({0, 1.0});
  c2.linearTerms.push_back({1, 3.0});
  model.constraints.push_back(c2);

  return model;
}

void test_root_relaxation_is_fractional() {
  const model::Model model = makeFractionalModel();

  milp::DualSimplexSolver solver;
  const milp::DualSimplexResult root = solver.solve(model);

  assert(root.status == milp::DualSimplexStatus::Optimal);
  assert(approx(root.objectiveValue, -11.0));
  assert(approx(root.primal[0], 1.8));
  assert(approx(root.primal[1], 1.4));

  std::cout << "[PASS] Test 1: Root relaxation is fractional (sanity check on the fixture)"
            << std::endl;
}

void test_generates_violated_cuts() {
  const model::Model model = makeFractionalModel();

  milp::DualSimplexSolver solver;
  const milp::DualSimplexResult root = solver.solve(model);
  assert(root.status == milp::DualSimplexStatus::Optimal);

  milp::GomoryCutGenerator generator;
  const std::vector<milp::Cut> cuts = generator.generate(model, root);

  // Both x and y are basic, integer-typed, and fractional at the root, so
  // this should generate one candidate cut per row.
  assert(!cuts.empty());

  for (const auto &cut : cuts) {
    assert(cut.coeffs.size() == model.variables.size());

    double lhs = 0.0;
    for (std::size_t j = 0; j < cut.coeffs.size(); ++j) {
      lhs += cut.coeffs[j] * root.primal[j];
    }
    // Every surviving cut must actually be violated by the current
    // fractional point (coeffs . x >= rhs is the cut's sense).
    assert(cut.rhs - lhs > 1e-4);
  }

  std::cout << "[PASS] Test 2: Generated cuts are violated by the fractional root point"
            << std::endl;
}

void test_cut_strictly_tightens_root_bound() {
  const model::Model model = makeFractionalModel();

  milp::DualSimplexSolver solver;
  const milp::DualSimplexResult root = solver.solve(model);
  assert(root.status == milp::DualSimplexStatus::Optimal);

  milp::GomoryCutGenerator generator;
  const std::vector<milp::Cut> cuts = generator.generate(model, root);
  assert(!cuts.empty());

  // Append every generated cut as a new row and re-solve from an extended
  // basis (new slacks start Basic, mirroring BranchAndBoundSolver's own
  // root cutting loop), exactly the mechanism under test in point 2 of the
  // task, exercised here directly rather than only through the full solve().
  model::Model augmented = model;
  milp::BasisState extendedBasis = root.basis.clone();
  for (std::size_t i = 0; i < cuts.size(); ++i) {
    model::Constraint row;
    row.name = "cut_" + std::to_string(i);
    row.lowerBound = cuts[i].rhs;
    row.upperBound = kInf;
    for (std::size_t j = 0; j < cuts[i].coeffs.size(); ++j) {
      if (cuts[i].coeffs[j] != 0.0) {
        row.linearTerms.push_back({static_cast<int>(j), cuts[i].coeffs[j]});
      }
    }
    augmented.constraints.push_back(std::move(row));
    extendedBasis.constraintStatus.push_back(milp::BasisStatus::Basic);
  }

  const milp::DualSimplexResult cutResult = solver.solveFromBasis(augmented, extendedBasis);
  assert(cutResult.status == milp::DualSimplexStatus::Optimal);

  // Minimizing: the relaxation objective is always <= the true integer
  // optimum (-10.0, hand-verified above), and a valid cut must strictly
  // increase it (move it closer to -10) without ever exceeding it.
  assert(cutResult.objectiveValue > root.objectiveValue + 1e-4);
  assert(cutResult.objectiveValue <= -10.0 + kTolerance);

  std::cout << "[PASS] Test 3: Adding the cut(s) strictly tightens the root LP bound"
            << std::endl;
}

// End-to-end regression check: BranchAndBoundSolver's own root cutting loop
// (point 2 of the task) must still land on the correct final integer
// optimum, not just tighten the root bound in isolation.
void test_end_to_end_with_cuts_still_correct() {
  const model::Model model = makeFractionalModel();

  milp::BranchAndBoundSolver solver;
  const milp::MilpResult result = solver.solve(model);

  assert(result.status == milp::MilpStatus::Optimal);
  assert(approx(result.objectiveValue, -10.0));
  assert(approx(result.primal[0], 2.0));
  assert(approx(result.primal[1], 1.0));

  std::cout << "[PASS] Test 4: End-to-end branch-and-cut still finds the true integer optimum"
            << std::endl;
}

// Cuts must never change the final answer, only (ideally) how the tree gets
// there. Disabling root cutting entirely (maxRootCutPasses = 0) must agree
// exactly with the default cuts-enabled run.
void test_cuts_disabled_agrees_with_cuts_enabled() {
  const model::Model model = makeFractionalModel();
  milp::BranchAndBoundSolver solver;

  milp::MilpOptions noCuts;
  noCuts.maxRootCutPasses = 0;
  const milp::MilpResult withoutCuts = solver.solve(model, noCuts);

  const milp::MilpResult withCuts = solver.solve(model, milp::MilpOptions{});

  assert(withoutCuts.status == milp::MilpStatus::Optimal);
  assert(withCuts.status == milp::MilpStatus::Optimal);
  assert(approx(withoutCuts.objectiveValue, withCuts.objectiveValue));
  assert(approx(withoutCuts.primal[0], withCuts.primal[0]));
  assert(approx(withoutCuts.primal[1], withCuts.primal[1]));

  std::cout << "[PASS] Test 5: Cuts-disabled and cuts-enabled runs agree on the final answer"
            << std::endl;
}

} // namespace

int main() {
  test_root_relaxation_is_fractional();
  test_generates_violated_cuts();
  test_cut_strictly_tightens_root_bound();
  test_end_to_end_with_cuts_still_correct();
  test_cuts_disabled_agrees_with_cuts_enabled();
  return 0;
}
