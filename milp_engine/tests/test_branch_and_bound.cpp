#include "milp/branch_and_bound.h"
#include "model/model.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>

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

// maximize 3x + 4y
// subject to 2x + y <= 5
//            x, y integer, in [0, 10]
//
// The user's own example. Its LP relaxation optimum (x=0, y=5, objective=20)
// happens to already be integer-feasible, so this exercises the cold-start
// -> integrality-check -> incumbent path directly, with zero branching.
void test_root_already_integer() {
  model::Model model;
  model.name = "root_already_integer";
  model.objective.sense = model::ObjectiveSense::Maximize;

  model.variables.push_back(makeIntegerVariable("x", 0.0, 10.0));
  model.variables.push_back(makeIntegerVariable("y", 0.0, 10.0));

  model.objective.linearTerms.push_back({0, 3.0});
  model.objective.linearTerms.push_back({1, 4.0});

  model::Constraint c1;
  c1.name = "c1";
  c1.lowerBound = -kInf;
  c1.upperBound = 5.0;
  c1.linearTerms.push_back({0, 2.0});
  c1.linearTerms.push_back({1, 1.0});
  model.constraints.push_back(c1);

  milp::BranchAndBoundSolver solver;
  const milp::MilpResult result = solver.solve(model);

  assert(result.status == milp::MilpStatus::Optimal);
  assert(approx(result.objectiveValue, 20.0));
  assert(approx(result.primal[0], 0.0));
  assert(approx(result.primal[1], 5.0));
  assert(result.nodeCount >= 1);

  std::cout << "[PASS] Test 1: Root relaxation already integer (no branching needed)"
            << std::endl;
}

// maximize 3x + 4y
// subject to 2x + y  <= 5
//            x + 3y <= 6
//            x, y integer, in [0, 10]
//
// The second constraint pulls the LP relaxation optimum to a fractional
// point (x=1.8, y=1.4, objective=11), forcing genuine branching. Hand
// enumeration of the small integer feasible region gives the true optimum
// at x=2, y=1, objective=10 (checked against every other integer point
// reachable under both constraints: (0,2)->8, (1,1)->7, (2,0)->6).
model::Model makeBranchingModel() {
  model::Model model;
  model.name = "forces_branching";
  model.objective.sense = model::ObjectiveSense::Maximize;

  model.variables.push_back(makeIntegerVariable("x", 0.0, 10.0));
  model.variables.push_back(makeIntegerVariable("y", 0.0, 10.0));

  model.objective.linearTerms.push_back({0, 3.0});
  model.objective.linearTerms.push_back({1, 4.0});

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

// Root-node Gomory cutting (added after this test was originally written) is
// now strong enough to close this particular small example's integer gap
// without any branching at all -- a good sign for the cuts, but it means
// this test's original nodeCount > 1 assertion no longer isolates what it's
// meant to test. Root cutting is disabled here so this test still exercises
// pure branching in isolation, as its name promises; cuts-enabled behavior
// on this same model is covered separately by
// milp_engine/tests/test_gomory_cuts.cpp.
void test_forces_branching() {
  const model::Model model = makeBranchingModel();

  milp::BranchAndBoundSolver solver;
  milp::MilpOptions options;
  options.maxRootCutPasses = 0;
  const milp::MilpResult result = solver.solve(model, options);

  assert(result.status == milp::MilpStatus::Optimal);
  assert(approx(result.objectiveValue, 10.0));
  assert(approx(result.primal[0], 2.0));
  assert(approx(result.primal[1], 1.0));
  assert(result.nodeCount > 1); // must have branched past the root

  std::cout << "[PASS] Test 2: Branching finds the true integer optimum" << std::endl;
}

// The same branching problem solved single-threaded and multi-threaded must
// agree exactly -- a guard against races in the shared incumbent/pruning
// logic that only manifest under real concurrency.
void test_single_and_multi_threaded_agree() {
  const model::Model model = makeBranchingModel();
  milp::BranchAndBoundSolver solver;

  milp::MilpOptions singleThreaded;
  singleThreaded.threadCount = 1;
  const milp::MilpResult single = solver.solve(model, singleThreaded);

  milp::MilpOptions multiThreaded;
  multiThreaded.threadCount = 4;
  const milp::MilpResult multi = solver.solve(model, multiThreaded);

  assert(single.status == milp::MilpStatus::Optimal);
  assert(multi.status == milp::MilpStatus::Optimal);
  assert(approx(single.objectiveValue, multi.objectiveValue));
  assert(approx(single.primal[0], multi.primal[0]));
  assert(approx(single.primal[1], multi.primal[1]));

  std::cout << "[PASS] Test 3: Single-threaded and multi-threaded solves agree" << std::endl;
}

// A model with no Integer/Binary variables at all should be accepted by the
// same integrality check (it never finds a branch variable) and resolved
// entirely at the root, with no branching attempted.
void test_continuous_only_model() {
  model::Model model;
  model.name = "continuous_only";

  model::Variable x;
  x.name = "x";
  x.lowerBound = 0.0;
  x.upperBound = 10.0;
  model.variables.push_back(x);

  model.objective.linearTerms.push_back({0, 1.0});

  model::Constraint c1;
  c1.name = "c1";
  c1.lowerBound = 2.5;
  c1.upperBound = kInf;
  c1.linearTerms.push_back({0, 1.0});
  model.constraints.push_back(c1);

  milp::BranchAndBoundSolver solver;
  const milp::MilpResult result = solver.solve(model);

  assert(result.status == milp::MilpStatus::Optimal);
  assert(approx(result.objectiveValue, 2.5));
  assert(approx(result.primal[0], 2.5));
  assert(result.nodeCount == 1); // resolved at the root, no branching possible

  std::cout << "[PASS] Test 4: Continuous-only model resolves at the root" << std::endl;
}

// A model with no integer-feasible point at all (x must be both >= 0.5 and
// <= 0.5 apart from its nearest integers, forced infeasible via bounds) is
// reported as Infeasible, not silently returning a fractional answer.
void test_no_integer_feasible_point() {
  model::Model model;
  model.name = "no_integer_point";

  model::Variable x;
  x.name = "x";
  x.type = model::VariableType::Integer;
  x.lowerBound = 0.5;
  x.upperBound = 0.9;
  model.variables.push_back(x);

  model.objective.linearTerms.push_back({0, 1.0});

  milp::BranchAndBoundSolver solver;
  const milp::MilpResult result = solver.solve(model);

  assert(result.status == milp::MilpStatus::Infeasible);

  std::cout << "[PASS] Test 5: No integer-feasible point reported as Infeasible" << std::endl;
}

} // namespace

int main() {
  test_root_already_integer();
  test_forces_branching();
  test_single_and_multi_threaded_agree();
  test_continuous_only_model();
  test_no_integer_feasible_point();
  return 0;
}
