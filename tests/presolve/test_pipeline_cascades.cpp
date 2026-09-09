#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "model/model.h"
#include "presolve/presolver.h"

namespace {

constexpr double INF = std::numeric_limits<double>::infinity();
constexpr double EPS = 1e-9;

bool approx(double a, double b) {
  if (std::isinf(a) || std::isinf(b)) {
    return a == b;
  }
  return std::abs(a - b) <= EPS;
}

model::Variable makeVar(const std::string &name, double lower = 0.0,
                        double upper = INF,
                        model::VariableType type = model::VariableType::Continuous) {
  model::Variable v;
  v.name = name;
  v.lowerBound = lower;
  v.upperBound = upper;
  v.type = type;
  return v;
}

model::Constraint makeCon(const std::string &name, double lower, double upper) {
  model::Constraint c;
  c.name = name;
  c.lowerBound = lower;
  c.upperBound = upper;
  return c;
}

// ============================================================
// 1. Singleton -> Fixed -> Empty Row Cascade (Cascade A)
// ============================================================
void test_cascade_singleton_fixed_empty_row() {
  model::Model m;
  m.name = "cascade_singleton_fixed_empty";
  m.variables.push_back(makeVar("x", 0.0, 10.0));
  m.variables.push_back(makeVar("y", 0.0, 10.0));

  // c0 (singleton): x = 4
  auto c0 = makeCon("c0_single", 4.0, 4.0);
  c0.linearTerms.push_back({0, 1.0});
  m.constraints.push_back(c0);

  // c1: x + y <= 12
  auto c1 = makeCon("c1", -INF, 12.0);
  c1.linearTerms.push_back({0, 1.0});
  c1.linearTerms.push_back({1, 1.0});
  m.constraints.push_back(c1);

  // c2: 2x <= 8 -> when x=4, becomes empty row 0 <= 0
  auto c2 = makeCon("c2_empty", -INF, 8.0);
  c2.linearTerms.push_back({0, 2.0});
  m.constraints.push_back(c2);

  presolve::Presolver presolver;
  auto res = presolver.run(m);

  assert(!res.infeasible);
  assert(res.converged);
  // x should be eliminated, only y survives
  assert(res.model.variables.size() == 1);
  assert(res.model.variables[0].name == "y");
  // Empty row c2 and singleton c0 should be removed
  // c1 becomes y <= 8, which is kept or further tightened
  assert(res.postsolve.fixedVariables.size() == 1);
  assert(res.postsolve.fixedVariables[0].name == "x");
  assert(approx(res.postsolve.fixedVariables[0].fixedValue, 4.0));
  assert(res.model.validate());
  std::cout << "[PASS] test_cascade_singleton_fixed_empty_row\n";
}

// ============================================================
// 2. Fixed Variable -> New Singleton Cascade (Cascade E)
// ============================================================
void test_cascade_fixed_new_singleton() {
  model::Model m;
  m.name = "cascade_fixed_new_singleton";
  m.variables.push_back(makeVar("x0", 5.0, 5.0)); // already fixed
  m.variables.push_back(makeVar("x1", 0.0, 10.0));
  m.variables.push_back(makeVar("x2", 0.0, 10.0));

  // c0: x0 + 2*x1 = 15 -> when x0=5, becomes 2*x1 = 10 (singleton!)
  auto c0 = makeCon("c0", 15.0, 15.0);
  c0.linearTerms.push_back({0, 1.0});
  c0.linearTerms.push_back({1, 2.0});
  m.constraints.push_back(c0);

  // c1: x1 + x2 <= 20
  auto c1 = makeCon("c1", -INF, 20.0);
  c1.linearTerms.push_back({1, 1.0});
  c1.linearTerms.push_back({2, 1.0});
  m.constraints.push_back(c1);

  presolve::Presolver presolver;
  auto res = presolver.run(m);

  assert(!res.infeasible);
  assert(res.converged);
  // Both x0 and x1 should be eliminated as fixed variables
  assert(res.model.variables.size() == 1);
  assert(res.model.variables[0].name == "x2");
  assert(res.postsolve.fixedVariables.size() == 2);
  // Verify both x0=5 and x1=5 were recorded
  bool foundX0 = false, foundX1 = false;
  for (const auto &fv : res.postsolve.fixedVariables) {
    if (fv.name == "x0") { foundX0 = true; assert(approx(fv.fixedValue, 5.0)); }
    if (fv.name == "x1") { foundX1 = true; assert(approx(fv.fixedValue, 5.0)); }
  }
  assert(foundX0 && foundX1);
  assert(res.model.validate());
  std::cout << "[PASS] test_cascade_fixed_new_singleton\n";
}

// ============================================================
// 3. Bound Tightening -> Redundant Constraint Cascade (Cascade B)
// ============================================================
void test_cascade_bound_tightening_redundant_constraint() {
  model::Model m;
  m.name = "cascade_bt_redundant";
  m.variables.push_back(makeVar("x", 0.0, 10.0));
  m.variables.push_back(makeVar("y", 0.0, 10.0));

  // c0: x + y >= 18 -> implies x >= 8, y >= 8
  auto c0 = makeCon("c0_tighten", 18.0, INF);
  c0.linearTerms.push_back({0, 1.0});
  c0.linearTerms.push_back({1, 1.0});
  m.constraints.push_back(c0);

  // c1: x + y >= 10 -> initially min activity is 0, but after tightening min activity is 16 >= 10 (redundant!)
  auto c1 = makeCon("c1_redundant", 10.0, INF);
  c1.linearTerms.push_back({0, 1.0});
  c1.linearTerms.push_back({1, 1.0});
  m.constraints.push_back(c1);

  presolve::Presolver presolver;
  auto res = presolver.run(m);

  assert(!res.infeasible);
  assert(res.converged);
  assert(res.model.variables.size() == 2);
  // x and y should be tightened to [8, 10]
  assert(approx(res.model.variables[0].lowerBound, 8.0));
  assert(approx(res.model.variables[1].lowerBound, 8.0));
  // c1 must be removed as redundant
  assert(res.model.constraints.size() == 1);
  assert(res.model.constraints[0].name == "c0_tighten");
  assert(res.model.validate());
  std::cout << "[PASS] test_cascade_bound_tightening_redundant_constraint\n";
}

// ============================================================
// 4. Integer Bound Tightening -> Fixed Variable (Cascade C)
// ============================================================
void test_cascade_integer_tightening_fixed_variable() {
  model::Model m;
  m.name = "cascade_int_tightening_fixed";
  m.variables.push_back(makeVar("x_int", 0.0, 10.0, model::VariableType::Integer));
  m.variables.push_back(makeVar("y_cont", 0.0, 10.0, model::VariableType::Continuous));

  // c0: 3*x + y <= 7.6 -> with y>=0, x <= 2.5333 -> floor to 2
  auto c0 = makeCon("c0", -INF, 7.6);
  c0.linearTerms.push_back({0, 3.0});
  c0.linearTerms.push_back({1, 1.0});
  m.constraints.push_back(c0);

  // c1: 2*x - y >= 3.8 -> with y<=0, wait, y in [0, 10]:
  // 2*x >= 3.8 + y >= 3.8 -> x >= 1.9 -> ceil to 2
  // So constraint is 2*x - y >= 3.8: wait, otherMax for y with coeff -1:
  // -1 * 0 = 0. So 2*x >= 3.8 - (-0) = 3.8 -> x >= 1.9. Ceil to 2!
  auto c1 = makeCon("c1", 3.8, INF);
  c1.linearTerms.push_back({0, 2.0});
  c1.linearTerms.push_back({1, -1.0});
  m.constraints.push_back(c1);

  presolve::Presolver presolver;
  auto res = presolver.run(m);

  assert(!res.infeasible);
  assert(res.converged);
  // x_int should become [2, 2], then fixed and eliminated!
  assert(res.model.variables.size() == 1);
  assert(res.model.variables[0].name == "y_cont");
  assert(res.postsolve.fixedVariables.size() == 1);
  assert(res.postsolve.fixedVariables[0].name == "x_int");
  assert(approx(res.postsolve.fixedVariables[0].fixedValue, 2.0));
  assert(res.model.validate());
  std::cout << "[PASS] test_cascade_integer_tightening_fixed_variable\n";
}

// ============================================================
// 5. Binary Bound Tightening -> Fixed Variable (Cascade D)
// ============================================================
void test_cascade_binary_tightening_fixed_variable() {
  model::Model m;
  m.name = "cascade_bin_tightening_fixed";
  m.variables.push_back(makeVar("b_bin", 0.0, 1.0, model::VariableType::Binary));
  m.variables.push_back(makeVar("y_cont", 0.0, 10.0, model::VariableType::Continuous));

  // c0: 2*b + y <= 1.6 -> with y>=0, b <= 0.8 -> floor to 0
  auto c0 = makeCon("c0", -INF, 1.6);
  c0.linearTerms.push_back({0, 2.0});
  c0.linearTerms.push_back({1, 1.0});
  m.constraints.push_back(c0);

  presolve::Presolver presolver;
  auto res = presolver.run(m);

  assert(!res.infeasible);
  assert(res.converged);
  // b_bin should become [0, 0], then fixed and eliminated!
  assert(res.model.variables.size() == 1);
  assert(res.model.variables[0].name == "y_cont");
  assert(res.postsolve.fixedVariables.size() == 1);
  assert(res.postsolve.fixedVariables[0].name == "b_bin");
  assert(approx(res.postsolve.fixedVariables[0].fixedValue, 0.0));
  assert(res.model.validate());
  std::cout << "[PASS] test_cascade_binary_tightening_fixed_variable\n";
}

// ============================================================
// 6. QP Cascade (Cascade F)
// ============================================================
void test_cascade_qp() {
  model::Model m;
  m.name = "cascade_qp";
  m.variables.push_back(makeVar("x0", 2.0, 2.0)); // fixed to 2
  m.variables.push_back(makeVar("x1", 0.0, 10.0));
  m.variables.push_back(makeVar("x2", 0.0, 10.0));

  m.objective.offset = 5.0;
  m.objective.linearTerms.push_back({1, 3.0}); // 3 * x1
  m.objective.linearTerms.push_back({2, 4.0}); // 4 * x2

  m.objective.quadraticTerms.push_back({0, 0, 2.0}); // 2 * x0^2
  m.objective.quadraticTerms.push_back({0, 1, 5.0}); // 5 * x0 * x1
  m.objective.quadraticTerms.push_back({1, 2, 6.0}); // 6 * x1 * x2

  // Constraint: x0 + x1 = 5 -> with x0=2, x1 becomes 3 (fixed!)
  auto c0 = makeCon("c0", 5.0, 5.0);
  c0.linearTerms.push_back({0, 1.0});
  c0.linearTerms.push_back({1, 1.0});
  m.constraints.push_back(c0);

  presolve::Presolver presolver;
  auto res = presolver.run(m);

  assert(!res.infeasible);
  assert(res.converged);
  // Both x0 and x1 are eliminated, only x2 survives!
  assert(res.model.variables.size() == 1);
  assert(res.model.variables[0].name == "x2");

  // Mathematical validation of offset and linear terms:
  // Step 1: x0 = 2 fixed:
  //   offset += 2 * 2^2 = 8 -> offset = 13
  //   cross term 5 * x0 * x1 -> 10 added to c_1: c_1 = 3 + 10 = 13
  //   c0 becomes x1 = 3 -> x1 fixed to 3!
  // Step 2: x1 = 3 fixed:
  //   offset += 13 * 3 = 39 -> offset = 13 + 39 = 52
  //   cross term 6 * x1 * x2 -> 18 added to c_2: c_2 = 4 + 18 = 22
  // Surviving variable x2: linear coefficient = 22.0
  // No remaining quadratic terms!
  assert(approx(res.model.objective.offset, 52.0));
  assert(res.model.objective.linearTerms.size() == 1);
  assert(res.model.objective.linearTerms[0].variableIndex == 0); // x2 is now index 0
  assert(approx(res.model.objective.linearTerms[0].value, 22.0));
  assert(res.model.objective.quadraticTerms.empty());
  assert(res.model.validate());
  std::cout << "[PASS] test_cascade_qp\n";
}

// ============================================================
// 7. Multiple Index Removals (Requirement 6)
// ============================================================
void test_multiple_index_removals() {
  model::Model m;
  m.name = "multiple_index_removals";
  m.variables.push_back(makeVar("x0", 10.0, 10.0)); // fixed
  m.variables.push_back(makeVar("x1", 0.0, 50.0));
  m.variables.push_back(makeVar("x2", 20.0, 20.0)); // fixed
  m.variables.push_back(makeVar("x3", 0.0, 50.0));
  m.variables.push_back(makeVar("x4", 30.0, 30.0)); // fixed

  // Constraint 0: x0 = 10 (singleton, redundant)
  auto c0 = makeCon("c0_single", 10.0, 10.0);
  c0.linearTerms.push_back({0, 1.0});
  m.constraints.push_back(c0);

  // Constraint 1: x1 + x3 <= 50
  auto c1 = makeCon("c1_surviving", -INF, 50.0);
  c1.linearTerms.push_back({1, 1.0});
  c1.linearTerms.push_back({3, 1.0});
  m.constraints.push_back(c1);

  // Constraint 2: x2 = 20 (singleton, redundant)
  auto c2 = makeCon("c2_single", 20.0, 20.0);
  c2.linearTerms.push_back({2, 1.0});
  m.constraints.push_back(c2);

  // Constraint 3: x1 - x3 = 5
  auto c3 = makeCon("c3_surviving", 5.0, 5.0);
  c3.linearTerms.push_back({1, 1.0});
  c3.linearTerms.push_back({3, -1.0});
  m.constraints.push_back(c3);

  presolve::Presolver presolver;
  auto res = presolver.run(m);

  assert(!res.infeasible);
  assert(res.converged);
  assert(res.model.variables.size() == 2);
  assert(res.model.variables[0].name == "x1");
  assert(res.model.variables[1].name == "x3");

  // Check bidirectional mappings
  assert(res.postsolve.presolvedToOriginalVar.size() == 2);
  assert(res.postsolve.presolvedToOriginalVar[0] == 1);
  assert(res.postsolve.presolvedToOriginalVar[1] == 3);

  assert(res.postsolve.originalToPresolvedVar.size() == 5);
  assert(res.postsolve.originalToPresolvedVar[0] == -1);
  assert(res.postsolve.originalToPresolvedVar[1] == 0);
  assert(res.postsolve.originalToPresolvedVar[2] == -1);
  assert(res.postsolve.originalToPresolvedVar[3] == 1);
  assert(res.postsolve.originalToPresolvedVar[4] == -1);

  // Check surviving constraints refer to indices 0 and 1
  assert(res.model.constraints.size() == 2);
  for (const auto &c : res.model.constraints) {
    for (const auto &t : c.linearTerms) {
      assert(t.variableIndex >= 0 && t.variableIndex < 2);
    }
  }
  assert(res.model.validate());
  std::cout << "[PASS] test_multiple_index_removals\n";
}

// ============================================================
// 8. Repeated Presolve / Idempotence (Requirement 5)
// ============================================================
void test_idempotence() {
  model::Model m;
  m.name = "idempotence_test";
  m.variables.push_back(makeVar("x0", 0.0, 10.0));
  m.variables.push_back(makeVar("x1", 3.0, 3.0)); // fixed
  m.variables.push_back(makeVar("x2", 0.0, 20.0, model::VariableType::Integer));
  m.variables.push_back(makeVar("x3", 0.0, 1.0, model::VariableType::Binary));

  m.objective.offset = 10.0;
  m.objective.linearTerms.push_back({0, 2.0});
  m.objective.linearTerms.push_back({1, 5.0});
  m.objective.linearTerms.push_back({2, 3.0});
  m.objective.quadraticTerms.push_back({0, 2, 1.5});

  // c0: x0 + x1 <= 15 -> becomes x0 <= 12
  auto c0 = makeCon("c0", -INF, 15.0);
  c0.linearTerms.push_back({0, 1.0});
  c0.linearTerms.push_back({1, 1.0});
  m.constraints.push_back(c0);

  // c1: 2*x2 + x3 <= 10
  auto c1 = makeCon("c1", -INF, 10.0);
  c1.linearTerms.push_back({2, 2.0});
  c1.linearTerms.push_back({3, 1.0});
  m.constraints.push_back(c1);

  presolve::Presolver presolver;
  auto res1 = presolver.run(m);
  assert(!res1.infeasible);
  assert(res1.converged);

  // Now run presolve on already-presolved model1
  auto model1 = res1.model;
  auto res2 = presolver.run(model1);

  assert(!res2.infeasible);
  assert(res2.converged);

  // Verify second presolve is completely stable
  const auto &m1 = res1.model;
  const auto &m2 = res2.model;

  assert(m1.variables.size() == m2.variables.size());
  for (std::size_t i = 0; i < m1.variables.size(); ++i) {
    assert(m1.variables[i].name == m2.variables[i].name);
    assert(m1.variables[i].type == m2.variables[i].type);
    assert(approx(m1.variables[i].lowerBound, m2.variables[i].lowerBound));
    assert(approx(m1.variables[i].upperBound, m2.variables[i].upperBound));
  }

  assert(m1.constraints.size() == m2.constraints.size());
  for (std::size_t i = 0; i < m1.constraints.size(); ++i) {
    assert(m1.constraints[i].name == m2.constraints[i].name);
    assert(approx(m1.constraints[i].lowerBound, m2.constraints[i].lowerBound));
    assert(approx(m1.constraints[i].upperBound, m2.constraints[i].upperBound));
    assert(m1.constraints[i].linearTerms.size() == m2.constraints[i].linearTerms.size());
    for (std::size_t t = 0; t < m1.constraints[i].linearTerms.size(); ++t) {
      assert(m1.constraints[i].linearTerms[t].variableIndex ==
             m2.constraints[i].linearTerms[t].variableIndex);
      assert(approx(m1.constraints[i].linearTerms[t].value,
                    m2.constraints[i].linearTerms[t].value));
    }
  }

  assert(approx(m1.objective.offset, m2.objective.offset));
  assert(m1.objective.linearTerms.size() == m2.objective.linearTerms.size());
  for (std::size_t i = 0; i < m1.objective.linearTerms.size(); ++i) {
    assert(m1.objective.linearTerms[i].variableIndex == m2.objective.linearTerms[i].variableIndex);
    assert(approx(m1.objective.linearTerms[i].value, m2.objective.linearTerms[i].value));
  }

  assert(m1.objective.quadraticTerms.size() == m2.objective.quadraticTerms.size());
  for (std::size_t i = 0; i < m1.objective.quadraticTerms.size(); ++i) {
    assert(m1.objective.quadraticTerms[i].variableIndex1 == m2.objective.quadraticTerms[i].variableIndex1);
    assert(m1.objective.quadraticTerms[i].variableIndex2 == m2.objective.quadraticTerms[i].variableIndex2);
    assert(approx(m1.objective.quadraticTerms[i].value, m2.objective.quadraticTerms[i].value));
  }

  // Second presolve should have performed 0 transformations
  assert(res2.transformations.empty());
  std::cout << "[PASS] test_idempotence\n";
}

// ============================================================
// 9. Infeasibility Discovered During Cascade (Requirement 8)
// ============================================================
void test_infeasibility_discovered_during_cascade() {
  model::Model m;
  m.name = "infeasible_in_cascade";
  m.variables.push_back(makeVar("x", 0.0, 10.0));
  m.variables.push_back(makeVar("y", 0.0, 10.0));

  // c0 (singleton): x = 5
  auto c0 = makeCon("c0_single", 5.0, 5.0);
  c0.linearTerms.push_back({0, 1.0});
  m.constraints.push_back(c0);

  // c1: x + y <= 3 -> when x becomes 5, becomes y <= -2, contradictory with y in [0, 10]
  auto c1 = makeCon("c1_contradict", -INF, 3.0);
  c1.linearTerms.push_back({0, 1.0});
  c1.linearTerms.push_back({1, 1.0});
  m.constraints.push_back(c1);

  presolve::Presolver presolver;
  auto res = presolver.run(m);

  assert(res.infeasible);
  std::cout << "[PASS] test_infeasibility_discovered_during_cascade\n";
}

// ============================================================
// 10. Order Independence of Safe Reductions (Requirement 7)
// ============================================================
void test_order_independence() {
  // Model A: x0 fixed to 2, x1 continuous, x2 fixed to 3
  model::Model mA;
  mA.name = "order_A";
  mA.variables.push_back(makeVar("x0", 2.0, 2.0));
  mA.variables.push_back(makeVar("x1", 0.0, 10.0));
  mA.variables.push_back(makeVar("x2", 3.0, 3.0));
  mA.objective.linearTerms.push_back({0, 1.0}); // 1*x0
  mA.objective.linearTerms.push_back({1, 2.0}); // 2*x1
  mA.objective.linearTerms.push_back({2, 4.0}); // 4*x2
  auto cA = makeCon("cA", -INF, 15.0);
  cA.linearTerms.push_back({0, 1.0});
  cA.linearTerms.push_back({1, 1.0});
  cA.linearTerms.push_back({2, 1.0});
  mA.constraints.push_back(cA);

  // Model B: same variables but reversed order: x2 fixed to 3, x1 continuous, x0 fixed to 2
  model::Model mB;
  mB.name = "order_B";
  mB.variables.push_back(makeVar("x2", 3.0, 3.0));
  mB.variables.push_back(makeVar("x1", 0.0, 10.0));
  mB.variables.push_back(makeVar("x0", 2.0, 2.0));
  mB.objective.linearTerms.push_back({0, 4.0}); // 4*x2
  mB.objective.linearTerms.push_back({1, 2.0}); // 2*x1
  mB.objective.linearTerms.push_back({2, 1.0}); // 1*x0
  auto cB = makeCon("cB", -INF, 15.0);
  cB.linearTerms.push_back({0, 1.0});
  cB.linearTerms.push_back({1, 1.0});
  cB.linearTerms.push_back({2, 1.0});
  mB.constraints.push_back(cB);

  presolve::Presolver presolver;
  auto resA = presolver.run(mA);
  auto resB = presolver.run(mB);

  assert(!resA.infeasible && !resB.infeasible);
  assert(resA.converged && resB.converged);

  // Both should reduce to 1 variable (x1) with bounds [0, 10]
  assert(resA.model.variables.size() == 1);
  assert(resB.model.variables.size() == 1);
  assert(resA.model.variables[0].name == "x1");
  assert(resB.model.variables[0].name == "x1");
  assert(approx(resA.model.variables[0].lowerBound, resB.model.variables[0].lowerBound));
  assert(approx(resA.model.variables[0].upperBound, resB.model.variables[0].upperBound));

  // Objective offset: 1*2 + 4*3 = 14 in both
  assert(approx(resA.model.objective.offset, 14.0));
  assert(approx(resB.model.objective.offset, 14.0));

  // Linear term on x1: 2.0 in both
  assert(resA.model.objective.linearTerms.size() == 1);
  assert(resB.model.objective.linearTerms.size() == 1);
  assert(approx(resA.model.objective.linearTerms[0].value, 2.0));
  assert(approx(resB.model.objective.linearTerms[0].value, 2.0));

  std::cout << "[PASS] test_order_independence\n";
}

// ============================================================
// 11. Stable Transformation Identities After Multiple Removals
// ============================================================
void test_stable_transformation_identities() {
  model::Model m;
  m.name = "stable_identities";
  m.variables.push_back(makeVar("v0", 0.0, 10.0));
  m.variables.push_back(makeVar("v1", 1.0, 1.0)); // fixed, orig 1
  m.variables.push_back(makeVar("v2", 0.0, 10.0));
  m.variables.push_back(makeVar("v3", 2.0, 2.0)); // fixed, orig 3
  m.variables.push_back(makeVar("v4", 0.0, 10.0));

  // Singleton on v0
  auto c0 = makeCon("c0_single_v0", 5.0, 5.0);
  c0.linearTerms.push_back({0, 1.0});
  m.constraints.push_back(c0);

  // Constraint on v2 and v4
  auto c1 = makeCon("c1_normal", -INF, 20.0);
  c1.linearTerms.push_back({2, 1.0});
  c1.linearTerms.push_back({4, 1.0});
  m.constraints.push_back(c1);

  presolve::Presolver presolver;
  auto res = presolver.run(m);

  assert(!res.infeasible);
  assert(res.converged);

  // v0 was tightened to [5, 5] by c0, then fixed to 5!
  // v1 was fixed to 1!
  // v3 was fixed to 2!
  // All 3 fixed variables should be in metadata with correct original indices
  assert(res.postsolve.fixedVariables.size() == 3);
  for (const auto &fv : res.postsolve.fixedVariables) {
    if (fv.name == "v0") {
      assert(fv.originalIndex == 0);
      assert(approx(fv.fixedValue, 5.0));
    } else if (fv.name == "v1") {
      assert(fv.originalIndex == 1);
      assert(approx(fv.fixedValue, 1.0));
    } else if (fv.name == "v3") {
      assert(fv.originalIndex == 3);
      assert(approx(fv.fixedValue, 2.0));
    } else {
      assert(false && "Unexpected fixed variable");
    }
  }

  // Surviving variables are v2 (orig 2) and v4 (orig 4)
  assert(res.model.variables.size() == 2);
  assert(res.postsolve.presolvedToOriginalVar[0] == 2);
  assert(res.postsolve.presolvedToOriginalVar[1] == 4);

  std::cout << "[PASS] test_stable_transformation_identities\n";
}

} // namespace

int main() {
  std::cout << "Running Pipeline & Cascading Reductions Tests...\n";
  test_cascade_singleton_fixed_empty_row();
  test_cascade_fixed_new_singleton();
  test_cascade_bound_tightening_redundant_constraint();
  test_cascade_integer_tightening_fixed_variable();
  test_cascade_binary_tightening_fixed_variable();
  test_cascade_qp();
  test_multiple_index_removals();
  test_idempotence();
  test_infeasibility_discovered_during_cascade();
  test_order_independence();
  test_stable_transformation_identities();

  std::cout << "All Pipeline & Cascade tests passed successfully!\n";
  return 0;
}
