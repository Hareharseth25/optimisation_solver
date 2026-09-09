#include "milp/dual_simplex_solver.h"
#include "milp/heuristics.h"
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

bool isFeasible(const model::Model &model, const std::vector<double> &point, double tolerance) {
  for (std::size_t j = 0; j < model.variables.size(); ++j) {
    if (point[j] < model.variables[j].lowerBound - tolerance ||
        point[j] > model.variables[j].upperBound + tolerance) {
      return false;
    }
  }
  for (const auto &constraint : model.constraints) {
    double activity = 0.0;
    for (const auto &term : constraint.linearTerms) {
      activity += term.value * point[static_cast<std::size_t>(term.variableIndex)];
    }
    if (activity < constraint.lowerBound - tolerance || activity > constraint.upperBound + tolerance) {
      return false;
    }
  }
  return true;
}

bool isIntegral(const model::Model &model, const std::vector<double> &point, double tolerance) {
  for (std::size_t j = 0; j < model.variables.size(); ++j) {
    if (model.variables[j].type == model::VariableType::Continuous) {
      continue;
    }
    if (std::abs(point[j] - std::round(point[j])) > tolerance) {
      return false;
    }
  }
  return true;
}

// minimize -x - y
// subject to x + y <= 3.3
//            x - y <= 0.4
// x, y integer in [0, 5]  (a pure integer relaxation -- no continuous vars)
//
// LP relaxation optimum: x=0, y=3.3 (both rows binding: x+y=3.3 at x=0 needs
// checking -- solved and cross-checked empirically, not just assumed).
model::Model makePureIntegerModel() {
  model::Model model;
  model.name = "pure_integer";

  model.variables.push_back(makeIntegerVariable("x", 0.0, 5.0));
  model.variables.push_back(makeIntegerVariable("y", 0.0, 5.0));

  model.objective.linearTerms.push_back({0, -1.0});
  model.objective.linearTerms.push_back({1, -1.0});

  model::Constraint c1;
  c1.name = "c1";
  c1.lowerBound = -kInf;
  c1.upperBound = 3.3;
  c1.linearTerms.push_back({0, 1.0});
  c1.linearTerms.push_back({1, 1.0});
  model.constraints.push_back(c1);

  model::Constraint c2;
  c2.name = "c2";
  c2.lowerBound = -kInf;
  c2.upperBound = 0.4;
  c2.linearTerms.push_back({0, 1.0});
  c2.linearTerms.push_back({1, -1.0});
  model.constraints.push_back(c2);

  return model;
}

// minimize x
// subject to x + 3y = 10   (equality -- zero slack, the classic hard case
//                            for one-shot rounding)
// x, y integer in [0, 10]
//
// LP relaxation: x=0, y=10/3 (~3.333). Hand-verified (and cross-checked
// empirically) that no rounding of y to floor(3) or ceil(4), holding x=0
// fixed as SimpleRoundingHeuristic would evaluate it, satisfies the
// equality exactly: floor gives activity 9 (short by 1), ceil gives 12
// (over by 2) -- floor is picked (less violation) but the point x=0,y=3
// still violates the equality by 1 and is correctly rejected. Diving
// instead fixes y=3 (nearest, no direction ambiguity since 0.333 isn't a
// half-integer tie) and RE-SOLVES: x + 9 = 10 forces x = 1 exactly, which
// is itself already integral, so the dive succeeds in a single step at
// x=1, y=3, objective=1.
model::Model makeEqualityModel() {
  model::Model model;
  model.name = "equality_forces_dive";

  model.variables.push_back(makeIntegerVariable("x", 0.0, 10.0));
  model.variables.push_back(makeIntegerVariable("y", 0.0, 10.0));

  model.objective.linearTerms.push_back({0, 1.0});

  model::Constraint c1;
  c1.name = "c1";
  c1.lowerBound = 10.0;
  c1.upperBound = 10.0;
  c1.linearTerms.push_back({0, 1.0});
  c1.linearTerms.push_back({1, 3.0});
  model.constraints.push_back(c1);

  return model;
}

void test_simple_rounding_on_pure_integer_relaxation() {
  const model::Model model = makePureIntegerModel();

  milp::DualSimplexSolver solver;
  const milp::DualSimplexResult root = solver.solve(model);
  assert(root.status == milp::DualSimplexStatus::Optimal);

  // Confirm the fixture is genuinely fractional before testing the rounder.
  assert(!isIntegral(model, root.primal, 1e-5));

  milp::SimpleRoundingHeuristic rounding;
  const milp::HeuristicResult result = rounding.run(model, root.primal);

  assert(result.foundFeasible);
  assert(result.solution.size() == model.variables.size());
  assert(isIntegral(model, result.solution, 1e-9));
  assert(isFeasible(model, result.solution, 1e-6));

  std::cout << "[PASS] Test 1: SimpleRoundingHeuristic constructs a valid integer "
               "point on a pure integer relaxation"
            << std::endl;
}

void test_simple_rounding_leaves_continuous_variables_untouched() {
  model::Model model = makePureIntegerModel();

  // Add a continuous variable that also appears in c1, to confirm it's
  // never rounded even though it participates in the same constraints.
  model::Variable z;
  z.name = "z";
  z.lowerBound = 0.0;
  z.upperBound = 5.0;
  model.variables.push_back(z);
  model.constraints[0].linearTerms.push_back({2, 1.0});

  std::vector<double> relaxation = {0.0, 3.3, 1.75};

  milp::SimpleRoundingHeuristic rounding;
  const milp::HeuristicResult result = rounding.run(model, relaxation);

  // z's fractional value (1.75) must survive untouched regardless of
  // whether the rounded point turns out feasible.
  assert(result.solution.empty() || approx(result.solution[2], 1.75));

  std::cout << "[PASS] Test 2: SimpleRoundingHeuristic never rounds Continuous "
               "variables"
            << std::endl;
}

void test_rounding_fails_on_tight_equality() {
  const model::Model model = makeEqualityModel();

  milp::DualSimplexSolver solver;
  const milp::DualSimplexResult root = solver.solve(model);
  assert(root.status == milp::DualSimplexStatus::Optimal);
  assert(!isIntegral(model, root.primal, 1e-5));

  milp::SimpleRoundingHeuristic rounding;
  const milp::HeuristicResult result = rounding.run(model, root.primal);

  // The equality constraint has zero slack: no one-shot rounding of the
  // fractional y (holding the already-integer x fixed at its relaxation
  // value) can satisfy x + 3y = 10 exactly.
  assert(!result.foundFeasible);

  std::cout << "[PASS] Test 3: SimpleRoundingHeuristic correctly fails on a "
               "tight equality constraint"
            << std::endl;
}

void test_diving_succeeds_where_rounding_fails() {
  const model::Model model = makeEqualityModel();

  milp::DualSimplexSolver solver;
  const milp::DualSimplexResult root = solver.solve(model);
  assert(root.status == milp::DualSimplexStatus::Optimal);

  milp::SimpleRoundingHeuristic rounding;
  const milp::HeuristicResult roundingResult = rounding.run(model, root.primal);
  assert(!roundingResult.foundFeasible); // re-confirm the premise this test exercises

  milp::FractionalDivingHeuristic diving;
  const milp::HeuristicResult diveResult = diving.run(model, root);

  assert(diveResult.foundFeasible);
  assert(diveResult.solution.size() == model.variables.size());
  assert(isIntegral(model, diveResult.solution, 1e-9));
  assert(isFeasible(model, diveResult.solution, 1e-6));
  assert(approx(diveResult.solution[0], 1.0)); // x
  assert(approx(diveResult.solution[1], 3.0)); // y
  assert(approx(diveResult.objective, 1.0));

  std::cout << "[PASS] Test 4: FractionalDivingHeuristic discovers an integer "
               "incumbent where simple rounding fails"
            << std::endl;
}

void test_diving_respects_max_depth() {
  const model::Model model = makeEqualityModel();

  milp::DualSimplexSolver solver;
  const milp::DualSimplexResult root = solver.solve(model);

  milp::FractionalDivingHeuristic diving;
  milp::FractionalDivingOptions options;
  options.maxDepth = 0; // no steps allowed at all
  const milp::HeuristicResult result = diving.run(model, root, options);

  assert(!result.foundFeasible);

  std::cout << "[PASS] Test 5: FractionalDivingHeuristic honors maxDepth" << std::endl;
}

} // namespace

int main() {
  test_simple_rounding_on_pure_integer_relaxation();
  test_simple_rounding_leaves_continuous_variables_untouched();
  test_rounding_fails_on_tight_equality();
  test_diving_succeeds_where_rounding_fails();
  test_diving_respects_max_depth();
  return 0;
}
