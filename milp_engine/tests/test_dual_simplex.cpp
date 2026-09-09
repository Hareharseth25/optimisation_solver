#include "milp/basis_state.h"
#include "milp/dual_simplex_solver.h"
#include "model/model.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kTolerance = 1e-6;

bool approx(double a, double b) { return std::abs(a - b) <= kTolerance; }

// minimize   x + 2y
// subject to x + y = 1
//            x, y >= 0
//
// Known optimum (cross-validated against pdlp_engine/examples/solve_example.cpp
// and the LP pipeline's end-to-end test, which solve the same problem):
// x = 1, y = 0, objective = 1.
model::Model makeBaseModel() {
  model::Model model;
  model.name = "dual_simplex_base";

  model::Variable x;
  x.name = "x";
  x.lowerBound = 0.0;
  x.upperBound = kInf;
  model.variables.push_back(x);

  model::Variable y;
  y.name = "y";
  y.lowerBound = 0.0;
  y.upperBound = kInf;
  model.variables.push_back(y);

  model.objective.linearTerms.push_back({0, 1.0});
  model.objective.linearTerms.push_back({1, 2.0});

  model::Constraint c1;
  c1.name = "c1";
  c1.lowerBound = 1.0;
  c1.upperBound = 1.0;
  c1.linearTerms.push_back({0, 1.0});
  c1.linearTerms.push_back({1, 1.0});
  model.constraints.push_back(c1);

  return model;
}

void test_cold_start() {
  const model::Model model = makeBaseModel();

  milp::DualSimplexSolver solver;
  const milp::DualSimplexResult result = solver.solve(model);

  assert(result.status == milp::DualSimplexStatus::Optimal);
  assert(result.primal.size() == 2);
  assert(approx(result.primal[0], 1.0)); // x
  assert(approx(result.primal[1], 0.0)); // y
  assert(approx(result.objectiveValue, 1.0));
  assert(result.basis.variableStatus.size() == 2);
  assert(result.basis.constraintStatus.size() == 1);

  std::cout << "[PASS] Test 1: Dual simplex cold start" << std::endl;
}

// Re-solves the same problem with x's upper bound tightened to 0.3, warm
// starting from the cold-start solve's optimal basis. x=1 from the parent
// basis now violates the new bound, forcing exactly one dual-simplex pivot
// (hand-verified): y enters, x leaves at its new upper bound.
//
//   y = 1 - x ranges over [0.7, 1] as x ranges over [0, 0.3]
//   objective = x + 2y = 2 - x, minimized by taking x as large as its new
//   bound allows: x = 0.3, y = 0.7, objective = 1.7.
void test_warm_start() {
  const model::Model baseModel = makeBaseModel();

  milp::DualSimplexSolver solver;
  const milp::DualSimplexResult coldResult = solver.solve(baseModel);
  assert(coldResult.status == milp::DualSimplexStatus::Optimal);

  model::Model tightenedModel = baseModel;
  tightenedModel.variables[0].upperBound = 0.3;

  const milp::DualSimplexResult warmResult =
      solver.solveFromBasis(tightenedModel, coldResult.basis);

  assert(warmResult.status == milp::DualSimplexStatus::Optimal);
  assert(approx(warmResult.primal[0], 0.3)); // x
  assert(approx(warmResult.primal[1], 0.7)); // y
  assert(approx(warmResult.objectiveValue, 1.7));

  std::cout << "[PASS] Test 2: Dual simplex warm start from a prior basis" << std::endl;
}

// The warm start above should reach optimality in exactly one pivot from
// the given basis, since only one row was primal-infeasible. Cross-checking
// the iteration count directly guards against a solve that "accidentally"
// reaches the right answer via a different, incorrect path.
void test_warm_start_pivot_count() {
  const model::Model baseModel = makeBaseModel();

  milp::DualSimplexSolver solver;
  const milp::DualSimplexResult coldResult = solver.solve(baseModel);

  model::Model tightenedModel = baseModel;
  tightenedModel.variables[0].upperBound = 0.3;

  const milp::DualSimplexResult warmResult =
      solver.solveFromBasis(tightenedModel, coldResult.basis);

  assert(warmResult.iterations == 1);

  std::cout << "[PASS] Test 3: Warm start converges in exactly one pivot" << std::endl;
}

// A trivially infeasible model (x fixed at 5, constraint requires x <= 1)
// should be detected as Infeasible with a nonempty Farkas ray, not silently
// mishandled.
void test_infeasible_detection() {
  model::Model model;
  model.name = "infeasible";

  model::Variable x;
  x.name = "x";
  x.lowerBound = 5.0;
  x.upperBound = 5.0;
  model.variables.push_back(x);

  model.objective.linearTerms.push_back({0, 1.0});

  model::Constraint c1;
  c1.name = "c1";
  c1.lowerBound = -kInf;
  c1.upperBound = 1.0;
  c1.linearTerms.push_back({0, 1.0});
  model.constraints.push_back(c1);

  milp::DualSimplexSolver solver;
  const milp::DualSimplexResult result = solver.solve(model);

  assert(result.status == milp::DualSimplexStatus::Infeasible);
  assert(result.dualFarkasRay.size() == 1);

  std::cout << "[PASS] Test 4: Infeasibility detected with a Farkas ray" << std::endl;
}

// A variable with a nonzero cost and no constraints at all, and no bound in
// its improving direction, is unbounded by construction.
void test_unbounded_detection() {
  model::Model model;
  model.name = "unbounded";

  model::Variable x;
  x.name = "x";
  x.lowerBound = -kInf;
  x.upperBound = kInf;
  model.variables.push_back(x);

  model.objective.linearTerms.push_back({0, 1.0});

  milp::DualSimplexSolver solver;
  const milp::DualSimplexResult result = solver.solve(model);

  assert(result.status == milp::DualSimplexStatus::Unbounded);

  std::cout << "[PASS] Test 5: Unboundedness detected" << std::endl;
}

// BasisState round-trips through serialize()/deserialize() and clone().
void test_basis_state_serialization() {
  milp::BasisState state;
  state.variableStatus = {milp::BasisStatus::Basic, milp::BasisStatus::AtLower,
                           milp::BasisStatus::AtUpper, milp::BasisStatus::Fixed,
                           milp::BasisStatus::Free};
  state.constraintStatus = {milp::BasisStatus::Fixed, milp::BasisStatus::Basic};

  const milp::BasisState cloned = state.clone();
  assert(cloned == state);

  const std::vector<std::uint8_t> bytes = state.serialize();
  const milp::BasisState restored = milp::BasisState::deserialize(bytes);
  assert(restored == state);

  std::cout << "[PASS] Test 6: BasisState clone and serialize round trip" << std::endl;
}

} // namespace

int main() {
  test_cold_start();
  test_warm_start();
  test_warm_start_pivot_count();
  test_infeasible_detection();
  test_unbounded_detection();
  test_basis_state_serialization();
  return 0;
}
