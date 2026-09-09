#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>

#include "model/model.h"
#include "presolve/presolver.h"

namespace {

constexpr double INF = std::numeric_limits<double>::infinity();
constexpr double EPS = 1e-9;

bool approx(double a, double b) { return std::abs(a - b) <= EPS; }

model::Variable makeVariable(const std::string &name, double lower = 0.0,
                             double upper = INF) {
  model::Variable v;
  v.name = name;
  v.lowerBound = lower;
  v.upperBound = upper;
  return v;
}

model::LinearTerm term(int variableIndex, double value) {
  return {variableIndex, value};
}

model::Constraint makeConstraint(const std::string &name, double lower,
                                 double upper) {
  model::Constraint c;
  c.name = name;
  c.lowerBound = lower;
  c.upperBound = upper;
  return c;
}

// ============================================================
// TEST 1 — Singleton >=
// ============================================================
//
// 2x >= 10
// x >= 0
//
// Expected:
// x >= 5
// constraint removed
//
void test_singleton_greater_than() {

  model::Model m;

  m.name = "singleton_ge";

  m.variables.push_back(makeVariable("x", 0.0, 100.0));

  auto c = makeConstraint("c1", 10.0, INF);
  c.linearTerms.push_back(term(0, 2.0));

  m.constraints.push_back(c);

  presolve::Presolver presolver;
  auto result = presolver.run(m);

  assert(!result.infeasible);
  assert(result.model.variables.size() == 1);

  assert(approx(result.model.variables[0].lowerBound, 5.0));
  assert(approx(result.model.variables[0].upperBound, 100.0));

  assert(result.model.constraints.empty());

  std::cout << "[PASS] Test 1: Singleton >=\n";
}

// ============================================================
// TEST 2 — Singleton <=
// ============================================================
//
// 3x <= 12
// x >= 0
//
// Expected:
// x <= 4
//
void test_singleton_less_than() {

  model::Model m;

  m.name = "singleton_le";

  m.variables.push_back(makeVariable("x", 0.0, 100.0));

  auto c = makeConstraint("c1", -INF, 12.0);
  c.linearTerms.push_back(term(0, 3.0));

  m.constraints.push_back(c);

  presolve::Presolver presolver;
  auto result = presolver.run(m);

  assert(!result.infeasible);

  assert(approx(result.model.variables[0].lowerBound, 0.0));
  assert(approx(result.model.variables[0].upperBound, 4.0));

  assert(result.model.constraints.empty());

  std::cout << "[PASS] Test 2: Singleton <=\n";
}

// ============================================================
// TEST 3 — Singleton with negative coefficient
// ============================================================
//
// -2x >= 10
//
// Divide by -2 and reverse inequality:
//
// x <= -5
//
// Existing x >= -100.
//
// Expected:
// [-100, -5]
//
void test_singleton_negative_coefficient() {

  model::Model m;

  m.name = "singleton_negative";

  m.variables.push_back(makeVariable("x", -100.0, 100.0));

  auto c = makeConstraint("c1", 10.0, INF);
  c.linearTerms.push_back(term(0, -2.0));

  m.constraints.push_back(c);

  presolve::Presolver presolver;
  auto result = presolver.run(m);

  assert(!result.infeasible);

  assert(approx(result.model.variables[0].lowerBound, -100.0));
  assert(approx(result.model.variables[0].upperBound, -5.0));

  assert(result.model.constraints.empty());

  std::cout << "[PASS] Test 3: Singleton negative coefficient\n";
}

// ============================================================
// TEST 4 — Singleton equality
// ============================================================
//
// 2x = 10
//
// Expected:
// x = 5
//
// This should eventually be recognized as a fixed variable.
//
void test_singleton_equality() {

  model::Model m;

  m.name = "singleton_eq";

  m.variables.push_back(makeVariable("x", 0.0, 100.0));

  auto c = makeConstraint("c1", 10.0, 10.0);
  c.linearTerms.push_back(term(0, 2.0));

  m.constraints.push_back(c);

  presolve::Presolver presolver;
  auto result = presolver.run(m);

  assert(!result.infeasible);

  assert(result.model.variables.empty());

  assert(result.model.constraints.empty());

  std::cout << "[PASS] Test 4: Singleton equality\n";
}

// ============================================================
// TEST 5 — Simple infeasibility
// ============================================================
//
// x ∈ [0, 10]
// x >= 15
//
// Expected:
// infeasible
//
void test_simple_infeasibility() {

  model::Model m;

  m.name = "simple_infeasible";

  m.variables.push_back(makeVariable("x", 0.0, 10.0));

  auto c = makeConstraint("c1", 15.0, INF);
  c.linearTerms.push_back(term(0, 1.0));

  m.constraints.push_back(c);

  presolve::Presolver presolver;
  auto result = presolver.run(m);

  assert(result.infeasible);

  std::cout << "[PASS] Test 5: Simple infeasibility\n";
}

// ============================================================
// TEST 6 — Bound tightening
// ============================================================
//
// x + y <= 10
// y >= 6
//
// Therefore:
//
// x <= 4
//
void test_bound_tightening() {

  model::Model m;

  m.name = "bound_tightening";

  m.variables.push_back(makeVariable("x", 0.0, 100.0));
  m.variables.push_back(makeVariable("y", 6.0, 100.0));

  auto c = makeConstraint("c1", -INF, 10.0);

  c.linearTerms.push_back(term(0, 1.0));
  c.linearTerms.push_back(term(1, 1.0));

  m.constraints.push_back(c);

  presolve::Presolver presolver;
  auto result = presolver.run(m);

  assert(!result.infeasible);

  assert(result.model.variables.size() == 2);

  assert(approx(result.model.variables[0].upperBound, 4.0));

  std::cout << "[PASS] Test 6: Bound tightening\n";
}

// ============================================================
// TEST 7 — Redundant constraint
// ============================================================
//
// x ∈ [0, 10]
//
// Constraint:
//
// x <= 20
//
// Since x can never exceed 10, the constraint is redundant.
//
void test_redundant_constraint() {

  model::Model m;

  m.name = "redundant";

  m.variables.push_back(makeVariable("x", 0.0, 10.0));

  auto c = makeConstraint("redundant_constraint", -INF, 20.0);
  c.linearTerms.push_back(term(0, 1.0));

  m.constraints.push_back(c);

  presolve::Presolver presolver;
  auto result = presolver.run(m);

  assert(!result.infeasible);
  assert(result.model.variables.size() == 1);
  assert(approx(result.model.variables[0].lowerBound, 0.0));
  assert(approx(result.model.variables[0].upperBound, 10.0));
  assert(result.model.constraints.empty());

  std::cout << "[PASS] Test 7: Redundant constraint\n";
}

// ============================================================
// TEST 8 — Non-redundant constraint must remain / tighten bound
// ============================================================
//
// x ∈ [0, 10]
//
// Constraint:
//
// x <= 5
//
// This is NOT redundant.
//
void test_non_redundant_constraint() {

  model::Model m;

  m.name = "non_redundant";

  m.variables.push_back(makeVariable("x", 0.0, 10.0));

  auto c = makeConstraint("important", -INF, 5.0);
  c.linearTerms.push_back(term(0, 1.0));

  m.constraints.push_back(c);

  presolve::Presolver presolver;
  auto result = presolver.run(m);

  assert(!result.infeasible);

  assert(result.model.variables.size() == 1);
  assert(approx(result.model.variables[0].lowerBound, 0.0));
  assert(approx(result.model.variables[0].upperBound, 5.0));
  assert(result.model.constraints.empty());

  std::cout << "[PASS] Test 8: Non-redundant constraint\n";
}

// ============================================================
// TEST 9 — Duplicate constraints
// ============================================================
//
// x <= 10
// x <= 10
//
// Both converted or duplicate removed, x <= 10 preserved.
//
void test_duplicate_constraints() {

  model::Model m;

  m.name = "duplicates";

  m.variables.push_back(makeVariable("x", 0.0, 100.0));

  auto c1 = makeConstraint("c1", -INF, 10.0);
  c1.linearTerms.push_back(term(0, 1.0));

  auto c2 = makeConstraint("c2", -INF, 10.0);
  c2.linearTerms.push_back(term(0, 1.0));

  m.constraints.push_back(c1);
  m.constraints.push_back(c2);

  presolve::Presolver presolver;
  auto result = presolver.run(m);

  assert(!result.infeasible);
  assert(result.model.variables.size() == 1);
  assert(approx(result.model.variables[0].upperBound, 10.0));
  assert(result.model.constraints.empty());

  std::cout << "[PASS] Test 9: Duplicate constraints\n";
}

// ============================================================
// TEST 10 — Parallel constraints
// ============================================================
//
// x <= 10
// 2x <= 30 (i.e. x <= 15)
//
// Presolver must preserve the stronger restriction x <= 10.
//
void test_parallel_constraints() {

  model::Model m;

  m.name = "parallel";

  m.variables.push_back(makeVariable("x", 0.0, 100.0));

  auto c1 = makeConstraint("c1", -INF, 10.0);
  c1.linearTerms.push_back(term(0, 1.0));

  auto c2 = makeConstraint("c2", -INF, 30.0);
  c2.linearTerms.push_back(term(0, 2.0));

  m.constraints.push_back(c1);
  m.constraints.push_back(c2);

  presolve::Presolver presolver;
  auto result = presolver.run(m);

  assert(!result.infeasible);

  assert(result.model.variables.size() == 1);
  assert(approx(result.model.variables[0].upperBound, 10.0));
  assert(result.model.constraints.empty());

  std::cout << "[PASS] Test 10: Parallel constraints\n";
}

} // namespace

int main() {

  std::cout << "\n========================================\n";
  std::cout << "BATCH 2 PRESOLVE TESTS\n";
  std::cout << "========================================\n\n";

  test_singleton_greater_than();
  test_singleton_less_than();
  test_singleton_negative_coefficient();
  test_singleton_equality();
  test_simple_infeasibility();
  test_bound_tightening();
  test_redundant_constraint();
  test_non_redundant_constraint();
  test_duplicate_constraints();
  test_parallel_constraints();

  std::cout << "\nAll Batch 2 tests completed.\n";

  return 0;
}