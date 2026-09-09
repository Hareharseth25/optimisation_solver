#include "milp/branching_rules.h"
#include "model/model.h"

#include <cassert>
#include <cmath>
#include <iostream>

namespace {

constexpr double kTolerance = 1e-9;

bool approx(double a, double b) { return std::abs(a - b) <= kTolerance; }

model::Variable makeIntegerVariable(const std::string &name, double lower, double upper) {
  model::Variable v;
  v.name = name;
  v.type = model::VariableType::Integer;
  v.lowerBound = lower;
  v.upperBound = upper;
  return v;
}

// (a) Pseudo-costs accurately accumulate degradation across mock branch
// iterations, including the "one side per call" sentinel convention
// (a negative value means "no observation this side") that
// BranchAndBoundSolver relies on since it evaluates each child independently.
void test_update_accumulates_correctly() {
  milp::PseudoCostTracker tracker(2);

  // Down-only observation: degradation 4.0 over frac 0.5 -> per-unit 8.0.
  tracker.update(0, 4.0, -1.0, 0.5);
  // Up-only observation: degradation 3.0 over (1-0.25)=0.75 -> per-unit 4.0.
  tracker.update(0, -1.0, 3.0, 0.25);
  // Both sides in one call: down per-unit 4.0, up per-unit 2.0.
  tracker.update(0, 2.0, 1.0, 0.5);

  const milp::PseudoCostStats stats = tracker.stats(0);
  assert(stats.psiDownCount == 2);
  assert(approx(stats.psiDownSum, 8.0 + 4.0));
  assert(stats.psiUpCount == 2);
  assert(approx(stats.psiUpSum, 4.0 + 2.0));

  // Variable 1 never observed: no history, all-zero stats.
  const milp::PseudoCostStats untouched = tracker.stats(1);
  assert(untouched.psiDownCount == 0);
  assert(untouched.psiUpCount == 0);
  assert(!tracker.hasHistory(1));

  // hasHistory requires BOTH sides -- a variable observed only downward
  // must not be treated as having "sufficient" history yet.
  milp::PseudoCostTracker oneSided(1);
  oneSided.update(0, 5.0, -1.0, 0.5);
  assert(!oneSided.hasHistory(0));

  std::cout << "[PASS] Test 1: update() accumulates degradation correctly, "
               "including the one-side-per-call convention"
            << std::endl;
}

// (b) Scoring prioritizes variables with higher observed objective impact,
// and selectBranchingVariable() picks that variable even over one that is
// (by raw fractional distance alone) more fractional.
void test_scoring_prioritizes_higher_impact() {
  milp::PseudoCostTracker tracker(2);

  // Variable 0: expensive to branch on either way (per-unit pseudo-cost 20
  // on both sides).
  tracker.update(0, 10.0, 10.0, 0.5);
  // Variable 1: cheap to branch on (per-unit pseudo-cost 2 on both sides).
  tracker.update(1, 1.0, 1.0, 0.5);

  // At the same 0.5 fractionality, variable 0's score must exceed
  // variable 1's -- direct check of the D-/D+/score formula.
  const double score0 = tracker.score(0, 2.5);
  const double score1 = tracker.score(1, 2.5);
  assert(approx(score0, 11.6)); // min(10,10) + 0.16*max(10,10) = 10 + 1.6
  assert(approx(score1, 1.16)); // min(1,1) + 0.16*max(1,1) = 1 + 0.16
  assert(score0 > score1);

  // Now make variable 1 the MORE fractional one (distance 0.5, maximal) and
  // variable 0 barely fractional (distance 0.1) -- under plain
  // most-fractional selection, variable 1 would win. Pseudo-cost history
  // must still prefer variable 0, since its degradation impact dominates.
  model::Model model;
  model.name = "scoring_test";
  model.variables.push_back(makeIntegerVariable("x0", 0.0, 10.0));
  model.variables.push_back(makeIntegerVariable("x1", 0.0, 10.0));

  const std::vector<double> primal = {2.1, 2.5}; // x0 barely fractional, x1 maximally fractional

  const int chosen = tracker.selectBranchingVariable(model, primal, 1e-5);
  assert(chosen == 0);

  std::cout << "[PASS] Test 2: Scoring and selection prioritize higher "
               "objective impact over raw fractionality"
            << std::endl;
}

// (c) With no history at all, selection falls back seamlessly to the
// most-fractional rule.
void test_fallback_to_most_fractional() {
  milp::PseudoCostTracker tracker(3);

  model::Model model;
  model.name = "fallback_test";
  model.variables.push_back(makeIntegerVariable("x0", 0.0, 10.0)); // frac 0.1 -> distance 0.1
  model.variables.push_back(makeIntegerVariable("x1", 0.0, 10.0)); // frac 0.5 -> distance 0.5 (most fractional)
  model.variables.push_back(makeIntegerVariable("x2", 0.0, 10.0)); // frac 0.8 -> distance 0.2

  const std::vector<double> primal = {2.1, 3.5, 1.8};

  const int chosen = tracker.selectBranchingVariable(model, primal, 1e-5);
  assert(chosen == 1);

  // An already-integer relaxation has nothing to branch on.
  const std::vector<double> integerPrimal = {2.0, 3.0, 1.0};
  const int noneChosen = tracker.selectBranchingVariable(model, integerPrimal, 1e-5);
  assert(noneChosen == -1);

  std::cout << "[PASS] Test 3: Falls back to most-fractional selection with "
               "no pseudo-cost history"
            << std::endl;
}

// selectBranchingVariable must also skip Continuous variables entirely, even
// when they're fractional.
void test_continuous_variables_never_selected() {
  milp::PseudoCostTracker tracker(2);

  model::Model model;
  model.name = "continuous_skip_test";

  model::Variable integerVar = makeIntegerVariable("x", 0.0, 10.0);
  model.variables.push_back(integerVar);

  model::Variable continuousVar;
  continuousVar.name = "z";
  continuousVar.lowerBound = 0.0;
  continuousVar.upperBound = 10.0;
  model.variables.push_back(continuousVar); // type defaults to Continuous

  const std::vector<double> primal = {2.5, 7.7}; // both fractional

  const int chosen = tracker.selectBranchingVariable(model, primal, 1e-5);
  assert(chosen == 0);

  std::cout << "[PASS] Test 4: Continuous variables are never selected for "
               "branching"
            << std::endl;
}

} // namespace

int main() {
  test_update_accumulates_correctly();
  test_scoring_prioritizes_higher_impact();
  test_fallback_to_most_fractional();
  test_continuous_variables_never_selected();
  return 0;
}
