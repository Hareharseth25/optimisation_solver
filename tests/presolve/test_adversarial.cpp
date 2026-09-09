#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
#include <random>
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
// 1. EMPTY / DEGENERATE MODELS
// ============================================================
void test_empty_and_degenerate_models() {
  presolve::Presolver presolver;

  // Case 1: 0 variables, 0 constraints
  {
    model::Model m;
    m.name = "empty_model";
    auto res = presolver.run(m);
    assert(!res.infeasible);
    assert(res.converged);
    assert(res.model.variables.empty());
    assert(res.model.constraints.empty());
    assert(res.model.validate());
  }

  // Case 2: variables but no constraints
  {
    model::Model m;
    m.name = "vars_no_cons";
    m.variables.push_back(makeVar("x0", 0.0, 10.0));
    m.variables.push_back(makeVar("x1", 5.0, 5.0)); // fixed
    m.variables.push_back(makeVar("x2", -INF, INF));
    m.objective.linearTerms.push_back({0, 2.0});
    m.objective.linearTerms.push_back({1, 3.0});
    auto res = presolver.run(m);
    assert(!res.infeasible);
    assert(res.converged);
    assert(res.model.variables.size() == 2); // x1 fixed & eliminated
    assert(approx(res.model.objective.offset, 15.0));
    assert(res.model.validate());
  }

  // Case 3: completely empty objective
  {
    model::Model m;
    m.name = "empty_obj";
    m.variables.push_back(makeVar("x", 0.0, 10.0));
    auto res = presolver.run(m);
    assert(!res.infeasible);
    assert(res.converged);
    assert(res.model.validate());
  }

  // Case 4: objective with only zero coefficients
  {
    model::Model m;
    m.name = "zero_obj";
    m.variables.push_back(makeVar("x", 0.0, 10.0));
    m.objective.linearTerms.push_back({0, 0.0});
    m.objective.linearTerms.push_back({0, 1e-11});
    m.objective.quadraticTerms.push_back({0, 0, 0.0});
    auto res = presolver.run(m);
    assert(!res.infeasible);
    assert(res.converged);
    assert(res.model.objective.linearTerms.empty());
    assert(res.model.objective.quadraticTerms.empty());
    assert(res.model.validate());
  }

  // Case 5: empty constraints with various bounds
  // 0 <= 0 (feasible, redundant)
  {
    model::Model m;
    m.name = "c_0_le_0";
    auto c = makeCon("c", -INF, 0.0);
    m.constraints.push_back(c);
    auto res = presolver.run(m);
    assert(!res.infeasible);
    assert(res.model.constraints.empty());
  }

  // 0 <= 5 (feasible, redundant)
  {
    model::Model m;
    m.name = "c_0_le_5";
    auto c = makeCon("c", -INF, 5.0);
    m.constraints.push_back(c);
    auto res = presolver.run(m);
    assert(!res.infeasible);
    assert(res.model.constraints.empty());
  }

  // 0 >= 0 (feasible, redundant)
  {
    model::Model m;
    m.name = "c_0_ge_0";
    auto c = makeCon("c", 0.0, INF);
    m.constraints.push_back(c);
    auto res = presolver.run(m);
    assert(!res.infeasible);
    assert(res.model.constraints.empty());
  }

  // 0 >= 5 (infeasible!)
  {
    model::Model m;
    m.name = "c_0_ge_5";
    auto c = makeCon("c", 5.0, INF);
    m.constraints.push_back(c);
    auto res = presolver.run(m);
    assert(res.infeasible);
  }

  // 0 = 0 (feasible, redundant)
  {
    model::Model m;
    m.name = "c_0_eq_0";
    auto c = makeCon("c", 0.0, 0.0);
    m.constraints.push_back(c);
    auto res = presolver.run(m);
    assert(!res.infeasible);
    assert(res.model.constraints.empty());
  }

  // 0 = 5 (infeasible!)
  {
    model::Model m;
    m.name = "c_0_eq_5";
    auto c = makeCon("c", 5.0, 5.0);
    m.constraints.push_back(c);
    auto res = presolver.run(m);
    assert(res.infeasible);
  }

  // 0 <= -5 (infeasible!)
  {
    model::Model m;
    m.name = "c_0_le_minus5";
    auto c = makeCon("c", -INF, -5.0);
    m.constraints.push_back(c);
    auto res = presolver.run(m);
    assert(res.infeasible);
  }

  std::cout << "[PASS] test_empty_and_degenerate_models\n";
}

// ============================================================
// 2. BOUND EDGE CASES
// ============================================================
void test_bound_edge_cases() {
  presolve::Presolver presolver;

  // Case 1: lower == upper
  {
    model::Model m;
    m.name = "equal_bounds";
    m.variables.push_back(makeVar("x", 4.2, 4.2));
    auto res = presolver.run(m);
    assert(!res.infeasible);
    assert(res.model.variables.empty());
    assert(res.postsolve.fixedVariables.size() == 1);
    assert(approx(res.postsolve.fixedVariables[0].fixedValue, 4.2));
  }

  // Case 2: lower > upper (infeasible)
  {
    model::Model m;
    m.name = "crossed_bounds";
    m.variables.push_back(makeVar("x", 5.0, 4.0));
    auto res = presolver.run(m);
    assert(res.infeasible);
  }

  // Case 3: very small bound gap within EPS (fixed)
  {
    model::Model m;
    m.name = "tiny_gap";
    m.variables.push_back(makeVar("x", 1.0, 1.0 + 1e-10));
    auto res = presolver.run(m);
    assert(!res.infeasible);
    assert(res.model.variables.empty()); // fixed
    assert(res.postsolve.fixedVariables.size() == 1);
  }

  // Case 4: gap larger than EPS (not fixed)
  {
    model::Model m;
    m.name = "gap_above_eps";
    m.variables.push_back(makeVar("x", 1.0, 1.0 + 1e-7));
    auto res = presolver.run(m);
    assert(!res.infeasible);
    assert(res.model.variables.size() == 1); // retained
  }

  // Case 5: infinite lower bound, infinite upper bound, both infinite
  {
    model::Model m;
    m.name = "inf_bounds";
    m.variables.push_back(makeVar("x_free", -INF, INF));
    m.variables.push_back(makeVar("x_no_lb", -INF, 10.0));
    m.variables.push_back(makeVar("x_no_ub", 0.0, INF));
    auto res = presolver.run(m);
    assert(!res.infeasible);
    assert(res.model.variables.size() == 3);
    assert(res.model.validate());
  }

  // Case 6: very large finite bounds
  {
    model::Model m;
    m.name = "large_bounds";
    m.variables.push_back(makeVar("x_big", -1e14, 1e14));
    auto res = presolver.run(m);
    assert(!res.infeasible);
    assert(res.model.variables.size() == 1);
  }

  // Case 7: negative bounds
  {
    model::Model m;
    m.name = "neg_bounds";
    m.variables.push_back(makeVar("x_neg", -10.0, -2.0));
    auto res = presolver.run(m);
    assert(!res.infeasible);
    assert(res.model.variables.size() == 1);
    assert(approx(res.model.variables[0].lowerBound, -10.0));
    assert(approx(res.model.variables[0].upperBound, -2.0));
  }

  std::cout << "[PASS] test_bound_edge_cases\n";
}

// ============================================================
// 3. INTEGER / BINARY EDGE CASES
// ============================================================
void test_integer_binary_edge_cases() {
  presolve::Presolver presolver;

  // Integer [2.0, 2.0] -> fixed to 2
  {
    model::Model m;
    m.name = "int_2_2";
    m.variables.push_back(makeVar("x", 2.0, 2.0, model::VariableType::Integer));
    auto res = presolver.run(m);
    assert(!res.infeasible);
    assert(res.model.variables.empty());
    assert(res.postsolve.fixedVariables.size() == 1);
    assert(approx(res.postsolve.fixedVariables[0].fixedValue, 2.0));
  }

  // Integer [2.000000001, 2.999999999] -> feasible (contains 2 or 3)
  {
    model::Model m;
    m.name = "int_near_integers";
    m.variables.push_back(makeVar("x", 2.000000001, 2.999999999, model::VariableType::Integer));
    auto res = presolver.run(m);
    assert(!res.infeasible);
  }

  // Integer [2.2, 2.8] -> infeasible (no integer in interval!)
  {
    model::Model m;
    m.name = "int_no_int";
    m.variables.push_back(makeVar("x", 2.2, 2.8, model::VariableType::Integer));
    auto res = presolver.run(m);
    assert(res.infeasible);
  }

  // Integer [2.2, 3.8] via singleton constraint -> tightened to [3.0, 3.0], then fixed to 3
  {
    model::Model m;
    m.name = "int_single_integer";
    m.variables.push_back(makeVar("x", 0.0, 10.0, model::VariableType::Integer));
    auto c0 = makeCon("c0", 2.2, 3.8);
    c0.linearTerms.push_back({0, 1.0});
    m.constraints.push_back(c0);
    auto res = presolver.run(m);
    assert(!res.infeasible);
    assert(res.model.variables.empty());
    assert(res.postsolve.fixedVariables.size() == 1);
    assert(approx(res.postsolve.fixedVariables[0].fixedValue, 3.0));
  }

  // Integer [-3.8, -2.2] via singleton constraint -> tightened to [-3.0, -3.0], fixed to -3
  {
    model::Model m;
    m.name = "int_neg_single";
    m.variables.push_back(makeVar("x", -10.0, 0.0, model::VariableType::Integer));
    auto c0 = makeCon("c0", -3.8, -2.2);
    c0.linearTerms.push_back({0, 1.0});
    m.constraints.push_back(c0);
    auto res = presolver.run(m);
    assert(!res.infeasible);
    assert(res.model.variables.empty());
    assert(res.postsolve.fixedVariables.size() == 1);
    assert(approx(res.postsolve.fixedVariables[0].fixedValue, -3.0));
  }

  // Integer [-2.0, -2.0] -> fixed to -2
  {
    model::Model m;
    m.name = "int_neg2_neg2";
    m.variables.push_back(makeVar("x", -2.0, -2.0, model::VariableType::Integer));
    auto res = presolver.run(m);
    assert(!res.infeasible);
    assert(res.model.variables.empty());
    assert(res.postsolve.fixedVariables.size() == 1);
    assert(approx(res.postsolve.fixedVariables[0].fixedValue, -2.0));
  }

  // Binary bounds:
  // Binary [0.0, 0.0] -> fixed to 0
  {
    model::Model m;
    m.name = "bin_0";
    m.variables.push_back(makeVar("b", 0.0, 0.0, model::VariableType::Binary));
    auto res = presolver.run(m);
    assert(!res.infeasible);
    assert(res.model.variables.empty());
    assert(approx(res.postsolve.fixedVariables[0].fixedValue, 0.0));
  }

  // Binary [1.0, 1.0] -> fixed to 1
  {
    model::Model m;
    m.name = "bin_1";
    m.variables.push_back(makeVar("b", 1.0, 1.0, model::VariableType::Binary));
    auto res = presolver.run(m);
    assert(!res.infeasible);
    assert(res.model.variables.empty());
    assert(approx(res.postsolve.fixedVariables[0].fixedValue, 1.0));
  }

  // Binary [0.5, 0.5] -> infeasible!
  {
    model::Model m;
    m.name = "bin_half";
    m.variables.push_back(makeVar("b", 0.5, 0.5, model::VariableType::Binary));
    auto res = presolver.run(m);
    assert(res.infeasible);
  }

  // Binary tightened to [0.8, 1.0] -> rounds to [1, 1], fixed to 1
  {
    model::Model m;
    m.name = "bin_tighten_1";
    m.variables.push_back(makeVar("b", 0.0, 1.0, model::VariableType::Binary));
    auto c = makeCon("c", 0.8, 1.0);
    c.linearTerms.push_back({0, 1.0});
    m.constraints.push_back(c);
    auto res = presolver.run(m);
    assert(!res.infeasible);
    assert(res.model.variables.empty());
    assert(approx(res.postsolve.fixedVariables[0].fixedValue, 1.0));
  }

  // Binary tightened to [0.0, 0.2] -> rounds to [0, 0], fixed to 0
  {
    model::Model m;
    m.name = "bin_tighten_0";
    m.variables.push_back(makeVar("b", 0.0, 1.0, model::VariableType::Binary));
    auto c = makeCon("c", 0.0, 0.2);
    c.linearTerms.push_back({0, 1.0});
    m.constraints.push_back(c);
    auto res = presolver.run(m);
    assert(!res.infeasible);
    assert(res.model.variables.empty());
    assert(approx(res.postsolve.fixedVariables[0].fixedValue, 0.0));
  }

  std::cout << "[PASS] test_integer_binary_edge_cases\n";
}

// ============================================================
// 4. SINGLETON SIGN TESTS
// ============================================================
void test_singleton_sign_tests() {
  presolve::Presolver presolver;

  // 1. a > 0, L finite, U finite: 2*x in [4, 10] -> x in [2, 5]
  {
    model::Model m;
    m.name = "sign_1";
    m.variables.push_back(makeVar("x", 0.0, 20.0));
    auto c = makeCon("c", 4.0, 10.0);
    c.linearTerms.push_back({0, 2.0});
    m.constraints.push_back(c);
    auto res = presolver.run(m);
    assert(!res.infeasible);
    assert(res.model.variables.size() == 1);
    assert(approx(res.model.variables[0].lowerBound, 2.0));
    assert(approx(res.model.variables[0].upperBound, 5.0));
  }

  // 2. a > 0, L = -inf, U finite: 3*x <= 15 -> x <= 5
  {
    model::Model m;
    m.name = "sign_2";
    m.variables.push_back(makeVar("x", 0.0, 20.0));
    auto c = makeCon("c", -INF, 15.0);
    c.linearTerms.push_back({0, 3.0});
    m.constraints.push_back(c);
    auto res = presolver.run(m);
    assert(!res.infeasible);
    assert(approx(res.model.variables[0].upperBound, 5.0));
  }

  // 3. a > 0, L finite, U = +inf: 4*x >= 8 -> x >= 2
  {
    model::Model m;
    m.name = "sign_3";
    m.variables.push_back(makeVar("x", 0.0, 20.0));
    auto c = makeCon("c", 8.0, INF);
    c.linearTerms.push_back({0, 4.0});
    m.constraints.push_back(c);
    auto res = presolver.run(m);
    assert(!res.infeasible);
    assert(approx(res.model.variables[0].lowerBound, 2.0));
  }

  // 4. a < 0, L finite, U finite: -2*x in [-10, -4] -> x in [2, 5]
  {
    model::Model m;
    m.name = "sign_4";
    m.variables.push_back(makeVar("x", 0.0, 20.0));
    auto c = makeCon("c", -10.0, -4.0);
    c.linearTerms.push_back({0, -2.0});
    m.constraints.push_back(c);
    auto res = presolver.run(m);
    assert(!res.infeasible);
    assert(approx(res.model.variables[0].lowerBound, 2.0));
    assert(approx(res.model.variables[0].upperBound, 5.0));
  }

  // 5. a < 0, L = -inf, U finite: -3*x <= -6 -> x >= 2
  {
    model::Model m;
    m.name = "sign_5";
    m.variables.push_back(makeVar("x", 0.0, 20.0));
    auto c = makeCon("c", -INF, -6.0);
    c.linearTerms.push_back({0, -3.0});
    m.constraints.push_back(c);
    auto res = presolver.run(m);
    assert(!res.infeasible);
    assert(approx(res.model.variables[0].lowerBound, 2.0));
  }

  // 6. a < 0, L finite, U = +inf: -4*x >= -20 -> x <= 5
  {
    model::Model m;
    m.name = "sign_6";
    m.variables.push_back(makeVar("x", 0.0, 20.0));
    auto c = makeCon("c", -20.0, INF);
    c.linearTerms.push_back({0, -4.0});
    m.constraints.push_back(c);
    auto res = presolver.run(m);
    assert(!res.infeasible);
    assert(approx(res.model.variables[0].upperBound, 5.0));
  }

  // 7. a < 0, equality: -5*x = -15 -> x = 3
  {
    model::Model m;
    m.name = "sign_7";
    m.variables.push_back(makeVar("x", 0.0, 20.0));
    auto c = makeCon("c", -15.0, -15.0);
    c.linearTerms.push_back({0, -5.0});
    m.constraints.push_back(c);
    auto res = presolver.run(m);
    assert(!res.infeasible);
    assert(res.model.variables.empty()); // fixed to 3
    assert(approx(res.postsolve.fixedVariables[0].fixedValue, 3.0));
  }

  // 8. Redundant inequality: initial x in [0, 5], constraint x <= 10 -> constraint removed
  {
    model::Model m;
    m.name = "sign_redundant";
    m.variables.push_back(makeVar("x", 0.0, 5.0));
    auto c = makeCon("c", -INF, 10.0);
    c.linearTerms.push_back({0, 1.0});
    m.constraints.push_back(c);
    auto res = presolver.run(m);
    assert(!res.infeasible);
    assert(res.model.constraints.empty());
    assert(approx(res.model.variables[0].upperBound, 5.0));
  }

  // 9. Contradictory inequality: initial x in [0, 5], constraint -x <= -10 (x >= 10) -> infeasible
  {
    model::Model m;
    m.name = "sign_contradict";
    m.variables.push_back(makeVar("x", 0.0, 5.0));
    auto c = makeCon("c", -INF, -10.0);
    c.linearTerms.push_back({0, -1.0});
    m.constraints.push_back(c);
    auto res = presolver.run(m);
    assert(res.infeasible);
  }

  std::cout << "[PASS] test_singleton_sign_tests\n";
}

// ============================================================
// 5. BOUND-TIGHTENING ADVERSARIAL CASES
// ============================================================
void test_bound_tightening_adversarial_cascade() {
  // Chain:
  // w in [0, 10], z in [0, 10], y in [0, 10], x in [0, 5] (all integer)
  // c0: w >= 2 (singleton)
  // c1: z - w >= 1 -> z >= 3
  // c2: y - z >= 1 -> y >= 4
  // c3: x - y >= 1 -> x >= 5
  // x in [5, 5] -> fixed to 5!
  // c4: x + s <= 4 with s in [0, 10] -> when x=5, s <= -1 -> infeasible!
  model::Model m;
  m.name = "bt_adversarial_chain";
  m.variables.push_back(makeVar("w", 0.0, 10.0, model::VariableType::Integer));
  m.variables.push_back(makeVar("z", 0.0, 10.0, model::VariableType::Integer));
  m.variables.push_back(makeVar("y", 0.0, 10.0, model::VariableType::Integer));
  m.variables.push_back(makeVar("x", 0.0, 5.0, model::VariableType::Integer));
  m.variables.push_back(makeVar("s", 0.0, 10.0, model::VariableType::Continuous));

  auto c0 = makeCon("c0", 2.0, INF);
  c0.linearTerms.push_back({0, 1.0}); // w >= 2
  m.constraints.push_back(c0);

  auto c1 = makeCon("c1", 1.0, INF);
  c1.linearTerms.push_back({1, 1.0});  // z
  c1.linearTerms.push_back({0, -1.0}); // -w >= 1
  m.constraints.push_back(c1);

  auto c2 = makeCon("c2", 1.0, INF);
  c2.linearTerms.push_back({2, 1.0});  // y
  c2.linearTerms.push_back({1, -1.0}); // -z >= 1
  m.constraints.push_back(c2);

  auto c3 = makeCon("c3", 1.0, INF);
  c3.linearTerms.push_back({3, 1.0});  // x
  c3.linearTerms.push_back({2, -1.0}); // -y >= 1
  m.constraints.push_back(c3);

  auto c4 = makeCon("c4", -INF, 4.0);
  c4.linearTerms.push_back({3, 1.0}); // x
  c4.linearTerms.push_back({4, 1.0}); // + s <= 4
  m.constraints.push_back(c4);

  presolve::Presolver presolver;
  auto res = presolver.run(m);

  // Infeasibility should be detected when the cascade fixes x to 5 and c4 becomes s <= -1
  assert(res.infeasible);
  std::cout << "[PASS] test_bound_tightening_adversarial_cascade\n";
}

// ============================================================
// 6. PARALLEL CONSTRAINT CASES (A, B, C, D, E)
// ============================================================
void test_parallel_constraints_all_cases() {
  presolve::Presolver presolver;

  // A: x + y <= 5, 2x + 2y <= 10 -> one may be removed
  {
    model::Model m;
    m.name = "parallel_A";
    m.variables.push_back(makeVar("x", 0.0, 10.0));
    m.variables.push_back(makeVar("y", 0.0, 10.0));
    auto c1 = makeCon("c1", -INF, 5.0);
    c1.linearTerms.push_back({0, 1.0});
    c1.linearTerms.push_back({1, 1.0});
    m.constraints.push_back(c1);
    auto c2 = makeCon("c2", -INF, 10.0);
    c2.linearTerms.push_back({0, 2.0});
    c2.linearTerms.push_back({1, 2.0});
    m.constraints.push_back(c2);
    auto res = presolver.run(m);
    assert(!res.infeasible);
    assert(res.model.constraints.size() == 1);
  }

  // B: x + y <= 5, 2x + 2y <= 12 -> both retained (not identical normalized bounds)
  {
    model::Model m;
    m.name = "parallel_B";
    m.variables.push_back(makeVar("x", 0.0, 10.0));
    m.variables.push_back(makeVar("y", 0.0, 10.0));
    auto c1 = makeCon("c1", -INF, 5.0);
    c1.linearTerms.push_back({0, 1.0});
    c1.linearTerms.push_back({1, 1.0});
    m.constraints.push_back(c1);
    auto c2 = makeCon("c2", -INF, 12.0);
    c2.linearTerms.push_back({0, 2.0});
    c2.linearTerms.push_back({1, 2.0});
    m.constraints.push_back(c2);
    auto res = presolver.run(m);
    assert(!res.infeasible);
    // Neither is duplicate (bounds 5 vs 6). Retained.
    assert(res.model.constraints.size() == 2);
  }

  // C: x + y <= 5, 2x + 2y >= 12 -> contradictory! (x+y <= 5 vs x+y >= 6)
  {
    model::Model m;
    m.name = "parallel_C";
    m.variables.push_back(makeVar("x", 0.0, 10.0));
    m.variables.push_back(makeVar("y", 0.0, 10.0));
    auto c1 = makeCon("c1", -INF, 5.0);
    c1.linearTerms.push_back({0, 1.0});
    c1.linearTerms.push_back({1, 1.0});
    m.constraints.push_back(c1);
    auto c2 = makeCon("c2", 12.0, INF);
    c2.linearTerms.push_back({0, 2.0});
    c2.linearTerms.push_back({1, 2.0});
    m.constraints.push_back(c2);
    auto res = presolver.run(m);
    assert(res.infeasible);
  }

  // D: x + y = 5, 2x + 2y = 10 -> one removed
  {
    model::Model m;
    m.name = "parallel_D";
    m.variables.push_back(makeVar("x", 0.0, 10.0));
    m.variables.push_back(makeVar("y", 0.0, 10.0));
    auto c1 = makeCon("c1", 5.0, 5.0);
    c1.linearTerms.push_back({0, 1.0});
    c1.linearTerms.push_back({1, 1.0});
    m.constraints.push_back(c1);
    auto c2 = makeCon("c2", 10.0, 10.0);
    c2.linearTerms.push_back({0, 2.0});
    c2.linearTerms.push_back({1, 2.0});
    m.constraints.push_back(c2);
    auto res = presolver.run(m);
    assert(!res.infeasible);
    assert(res.model.constraints.size() == 1);
  }

  // E: x + y = 5, 2x + 2y = 11 -> infeasible
  {
    model::Model m;
    m.name = "parallel_E";
    m.variables.push_back(makeVar("x", 0.0, 10.0));
    m.variables.push_back(makeVar("y", 0.0, 10.0));
    auto c1 = makeCon("c1", 5.0, 5.0);
    c1.linearTerms.push_back({0, 1.0});
    c1.linearTerms.push_back({1, 1.0});
    m.constraints.push_back(c1);
    auto c2 = makeCon("c2", 11.0, 11.0);
    c2.linearTerms.push_back({0, 2.0});
    c2.linearTerms.push_back({1, 2.0});
    m.constraints.push_back(c2);
    auto res = presolver.run(m);
    assert(res.infeasible);
  }

  std::cout << "[PASS] test_parallel_constraints_all_cases\n";
}

// ============================================================
// 7. REDUNDANT CONSTRAINT TESTS
// ============================================================
void test_redundant_constraints_comprehensive() {
  presolve::Presolver presolver;

  // 1. Obviously redundant
  {
    model::Model m;
    m.name = "obv_redundant";
    m.variables.push_back(makeVar("x", 0.0, 5.0));
    m.variables.push_back(makeVar("y", 0.0, 5.0));
    auto c = makeCon("c_redundant", -INF, 20.0);
    c.linearTerms.push_back({0, 1.0});
    c.linearTerms.push_back({1, 1.0});
    m.constraints.push_back(c);
    auto res = presolver.run(m);
    assert(!res.infeasible);
    assert(res.model.constraints.empty()); // max activity 10 <= 20
  }

  // 2. Not redundant
  {
    model::Model m;
    m.name = "not_redundant";
    m.variables.push_back(makeVar("x", 0.0, 5.0));
    m.variables.push_back(makeVar("y", 0.0, 5.0));
    auto c = makeCon("c_restrictive", -INF, 8.0);
    c.linearTerms.push_back({0, 1.0});
    c.linearTerms.push_back({1, 1.0});
    m.constraints.push_back(c);
    auto res = presolver.run(m);
    assert(!res.infeasible);
    assert(res.model.constraints.size() == 1); // max activity 10 > 8
  }

  // 3. Almost redundant near EPS
  {
    model::Model m;
    m.name = "near_eps_redundant";
    m.variables.push_back(makeVar("x", 0.0, 5.0));
    auto c = makeCon("c_eps", -INF, 5.0 + 1e-10);
    c.linearTerms.push_back({0, 1.0});
    m.constraints.push_back(c);
    auto res = presolver.run(m);
    assert(!res.infeasible);
    assert(res.model.constraints.empty()); // singleton/redundant
  }

  // 4. Redundant with infinite variable bounds
  {
    model::Model m;
    m.name = "inf_redundant";
    m.variables.push_back(makeVar("x", -INF, INF));
    auto c = makeCon("c_infinite_bounds", -INF, INF);
    c.linearTerms.push_back({0, 1.0});
    m.constraints.push_back(c);
    auto res = presolver.run(m);
    assert(!res.infeasible);
    assert(res.model.constraints.empty());
  }

  std::cout << "[PASS] test_redundant_constraints_comprehensive\n";
}

// ============================================================
// 8. FIXED VARIABLE + QP TESTS (Exhaustive)
// ============================================================
void test_qp_adversarial_exhaustive() {
  // Construct QP with:
  // - x0 in [2, 2] (fixed)
  // - x1 in [3, 3] (fixed)
  // - x2 in [0, 10] (surviving)
  // - x3 in [0, 10] (surviving)
  // Diagonal terms on fixed variables: 4*x0^2, 5*x1^2
  // Cross term between two fixed variables: 7*x0*x1
  // Cross term with fixed as 1st, surviving as 2nd: 6*x0*x2
  // Cross term with surviving as 1st, fixed as 2nd: 8*x3*x1
  // Diagonal term on surviving variable: 9*x2^2
  // Cross term on surviving variables: 10*x2*x3
  // Linear terms: 1*x0 + 2*x1 + 3*x2 + 4*x3
  // Offset: 100.0
  model::Model m;
  m.name = "qp_exhaustive";
  m.variables.push_back(makeVar("x0", 2.0, 2.0));
  m.variables.push_back(makeVar("x1", 3.0, 3.0));
  m.variables.push_back(makeVar("x2", 0.0, 10.0));
  m.variables.push_back(makeVar("x3", 0.0, 10.0));

  m.objective.offset = 100.0;
  m.objective.linearTerms.push_back({0, 1.0});
  m.objective.linearTerms.push_back({1, 2.0});
  m.objective.linearTerms.push_back({2, 3.0});
  m.objective.linearTerms.push_back({3, 4.0});

  m.objective.quadraticTerms.push_back({0, 0, 4.0});  // 4*x0^2
  m.objective.quadraticTerms.push_back({1, 1, 5.0});  // 5*x1^2
  m.objective.quadraticTerms.push_back({0, 1, 7.0});  // 7*x0*x1
  m.objective.quadraticTerms.push_back({0, 2, 6.0});  // 6*x0*x2
  m.objective.quadraticTerms.push_back({3, 1, 8.0});  // 8*x3*x1
  m.objective.quadraticTerms.push_back({2, 2, 9.0});  // 9*x2^2
  m.objective.quadraticTerms.push_back({2, 3, 10.0}); // 10*x2*x3

  presolve::Presolver presolver;
  auto res = presolver.run(m);

  assert(!res.infeasible);
  assert(res.converged);
  assert(res.model.variables.size() == 2);
  assert(res.model.variables[0].name == "x2");
  assert(res.model.variables[1].name == "x3");

  // Mathematical expectation:
  // Offset: 100 + [1(2) + 2(3)] + [4(4) + 5(9) + 7(6)] = 100 + 8 + 61 + 42 = 211.0
  assert(approx(res.model.objective.offset, 211.0));

  // Linear terms:
  // x2: 3 + 6(2) = 15.0
  // x3: 4 + 8(3) = 28.0
  assert(res.model.objective.linearTerms.size() == 2);
  double c2 = 0.0, c3 = 0.0;
  for (const auto &lt : res.model.objective.linearTerms) {
    if (lt.variableIndex == 0) c2 = lt.value;
    if (lt.variableIndex == 1) c3 = lt.value;
  }
  assert(approx(c2, 15.0));
  assert(approx(c3, 28.0));

  // Quadratic terms:
  // x2^2: 9.0 at (0, 0)
  // x2*x3: 10.0 at (0, 1)
  assert(res.model.objective.quadraticTerms.size() == 2);

  // Evaluate at test point (x2=1.5, x3=2.5)
  const double test_x2 = 1.5;
  const double test_x3 = 2.5;
  const double f_orig = 100.0 + (1.0 * 2.0 + 2.0 * 3.0 + 3.0 * test_x2 + 4.0 * test_x3) +
                        (4.0 * 4.0 + 5.0 * 9.0 + 7.0 * 6.0 + 6.0 * 2.0 * test_x2 +
                         8.0 * test_x3 * 3.0 + 9.0 * test_x2 * test_x2 + 10.0 * test_x2 * test_x3);

  double f_presolved = res.model.objective.offset;
  for (const auto &lt : res.model.objective.linearTerms) {
    const double val = (lt.variableIndex == 0) ? test_x2 : test_x3;
    f_presolved += lt.value * val;
  }
  for (const auto &qt : res.model.objective.quadraticTerms) {
    const double v1 = (qt.variableIndex1 == 0) ? test_x2 : test_x3;
    const double v2 = (qt.variableIndex2 == 0) ? test_x2 : test_x3;
    f_presolved += qt.value * v1 * v2;
  }

  assert(approx(f_orig, f_presolved));
  assert(res.model.validate());
  std::cout << "[PASS] test_qp_adversarial_exhaustive\n";
}

// ============================================================
// 9. INDEX-SHIFT ATTACKS
// ============================================================
void test_index_shift_attacks() {
  // 6 variables: x0, x1, x2, x3, x4, x5
  // Fix x0 = 1, x2 = 2, x5 = 5
  // Surviving: x1 (orig 1 -> presolved 0), x3 (orig 3 -> presolved 1), x4 (orig 4 -> presolved 2)
  model::Model m;
  m.name = "index_shift_attack";
  m.variables.push_back(makeVar("x0", 1.0, 1.0)); // fixed
  m.variables.push_back(makeVar("x1", 0.0, 10.0));
  m.variables.push_back(makeVar("x2", 2.0, 2.0)); // fixed
  m.variables.push_back(makeVar("x3", 0.0, 10.0));
  m.variables.push_back(makeVar("x4", 0.0, 10.0));
  m.variables.push_back(makeVar("x5", 5.0, 5.0)); // fixed

  m.objective.quadraticTerms.push_back({1, 3, 2.0}); // x1 * x3 -> presolved (0, 1)
  m.objective.quadraticTerms.push_back({3, 4, 3.0}); // x3 * x4 -> presolved (1, 2)
  m.objective.quadraticTerms.push_back({0, 4, 4.0}); // x0 * x4 -> folds into x4 (presolved 2)

  // Constraint referencing surviving variables
  auto c0 = makeCon("c0", -INF, 20.0);
  c0.linearTerms.push_back({1, 1.0}); // x1
  c0.linearTerms.push_back({3, 1.0}); // x3
  c0.linearTerms.push_back({4, 1.0}); // x4
  m.constraints.push_back(c0);

  presolve::Presolver presolver;
  auto res = presolver.run(m);

  assert(!res.infeasible);
  assert(res.converged);
  assert(res.model.variables.size() == 3);
  assert(res.model.variables[0].name == "x1");
  assert(res.model.variables[1].name == "x3");
  assert(res.model.variables[2].name == "x4");

  // Verify index mappings
  assert(res.postsolve.presolvedToOriginalVar[0] == 1);
  assert(res.postsolve.presolvedToOriginalVar[1] == 3);
  assert(res.postsolve.presolvedToOriginalVar[2] == 4);

  assert(res.postsolve.originalToPresolvedVar[0] == -1);
  assert(res.postsolve.originalToPresolvedVar[1] == 0);
  assert(res.postsolve.originalToPresolvedVar[2] == -1);
  assert(res.postsolve.originalToPresolvedVar[3] == 1);
  assert(res.postsolve.originalToPresolvedVar[4] == 2);
  assert(res.postsolve.originalToPresolvedVar[5] == -1);

  // Verify surviving quadratic terms
  assert(res.model.objective.quadraticTerms.size() == 2);
  for (const auto &qt : res.model.objective.quadraticTerms) {
    assert(qt.variableIndex1 >= 0 && qt.variableIndex1 < 3);
    assert(qt.variableIndex2 >= 0 && qt.variableIndex2 < 3);
  }

  assert(res.model.validate());
  std::cout << "[PASS] test_index_shift_attacks\n";
}

// ============================================================
// 10. TRANSFORMATION METADATA ATTACKS
// ============================================================
void test_transformation_metadata_attacks() {
  model::Model m;
  m.name = "meta_attacks";
  m.variables.push_back(makeVar("v0", 0.0, 10.0));
  m.variables.push_back(makeVar("v1", 2.0, 2.0)); // fixed
  m.variables.push_back(makeVar("v2", 0.0, 10.0));
  m.variables.push_back(makeVar("v3", 4.0, 4.0)); // fixed
  m.variables.push_back(makeVar("v4", 0.0, 10.0));

  // Singleton constraint on v0 -> tightens v0 to 5.0, then eliminated as fixed
  auto c0 = makeCon("c0_single", 5.0, 5.0);
  c0.linearTerms.push_back({0, 1.0});
  m.constraints.push_back(c0);

  // Constraint on v2 and v4
  auto c1 = makeCon("c1", -INF, 10.0);
  c1.linearTerms.push_back({2, 1.0});
  c1.linearTerms.push_back({4, 1.0});
  m.constraints.push_back(c1);

  // Duplicate of c1
  auto c2 = makeCon("c2_dup", -INF, 10.0);
  c2.linearTerms.push_back({2, 1.0});
  c2.linearTerms.push_back({4, 1.0});
  m.constraints.push_back(c2);

  presolve::Presolver presolver;
  auto res = presolver.run(m);

  assert(!res.infeasible);
  assert(res.converged);

  // Verify each fixed variable record contains stable original index
  assert(res.postsolve.fixedVariables.size() == 3);
  for (const auto &rec : res.postsolve.fixedVariables) {
    if (rec.name == "v0") {
      assert(rec.originalIndex == 0);
      assert(approx(rec.fixedValue, 5.0));
    } else if (rec.name == "v1") {
      assert(rec.originalIndex == 1);
      assert(approx(rec.fixedValue, 2.0));
    } else if (rec.name == "v3") {
      assert(rec.originalIndex == 3);
      assert(approx(rec.fixedValue, 4.0));
    }
  }

  // Verify removed constraint records
  bool foundSingleton = false;
  bool foundDuplicate = false;
  for (const auto &rec : res.postsolve.removedConstraints) {
    if (rec.wasSingleton) {
      foundSingleton = true;
      assert(rec.originalIndex == 0);
    }
    if (rec.wasDuplicate) {
      foundDuplicate = true;
      assert(rec.originalIndex == 2);
      assert(rec.duplicateOfOriginalIndex == 1);
    }
  }
  assert(foundSingleton && foundDuplicate);
  assert(res.model.validate());
  std::cout << "[PASS] test_transformation_metadata_attacks\n";
}

// ============================================================
// 11. IDEMPOTENCE ATTACK (Three Presolve Levels)
// ============================================================
void test_idempotence_three_levels() {
  model::Model m;
  m.name = "idempotence_3levels";
  m.variables.push_back(makeVar("x0", 0.0, 10.0));
  m.variables.push_back(makeVar("x1", 3.0, 3.0)); // fixed
  m.variables.push_back(makeVar("x2", 0.0, 20.0, model::VariableType::Integer));
  m.variables.push_back(makeVar("x3", 0.0, 1.0, model::VariableType::Binary));

  m.objective.offset = 10.0;
  m.objective.linearTerms.push_back({0, 2.0});
  m.objective.linearTerms.push_back({1, 5.0});
  m.objective.linearTerms.push_back({2, 3.0});
  m.objective.quadraticTerms.push_back({0, 2, 1.5});

  auto c0 = makeCon("c0", -INF, 15.0);
  c0.linearTerms.push_back({0, 1.0});
  c0.linearTerms.push_back({1, 1.0});
  m.constraints.push_back(c0);

  auto c1 = makeCon("c1", -INF, 10.0);
  c1.linearTerms.push_back({2, 2.0});
  c1.linearTerms.push_back({3, 1.0});
  m.constraints.push_back(c1);

  presolve::Presolver presolver;
  auto m1 = presolver.run(m);
  auto m2 = presolver.run(m1.model);
  auto m3 = presolver.run(m2.model);

  assert(!m1.infeasible && !m2.infeasible && !m3.infeasible);
  assert(m1.converged && m2.converged && m3.converged);

  // m2 and m3 must perform 0 transformations
  assert(m2.transformations.empty());
  assert(m3.transformations.empty());

  // Compare m1.model, m2.model, and m3.model
  assert(m1.model.variables.size() == m2.model.variables.size());
  assert(m2.model.variables.size() == m3.model.variables.size());

  for (std::size_t i = 0; i < m1.model.variables.size(); ++i) {
    assert(m1.model.variables[i].name == m2.model.variables[i].name);
    assert(m2.model.variables[i].name == m3.model.variables[i].name);
    assert(approx(m1.model.variables[i].lowerBound, m2.model.variables[i].lowerBound));
    assert(approx(m2.model.variables[i].lowerBound, m3.model.variables[i].lowerBound));
    assert(approx(m1.model.variables[i].upperBound, m2.model.variables[i].upperBound));
    assert(approx(m2.model.variables[i].upperBound, m3.model.variables[i].upperBound));
  }

  assert(m1.model.constraints.size() == m2.model.constraints.size());
  assert(m2.model.constraints.size() == m3.model.constraints.size());

  assert(approx(m1.model.objective.offset, m2.model.objective.offset));
  assert(approx(m2.model.objective.offset, m3.model.objective.offset));

  assert(m1.model.objective.linearTerms.size() == m2.model.objective.linearTerms.size());
  assert(m2.model.objective.linearTerms.size() == m3.model.objective.linearTerms.size());

  assert(m1.model.objective.quadraticTerms.size() == m2.model.objective.quadraticTerms.size());
  assert(m2.model.objective.quadraticTerms.size() == m3.model.objective.quadraticTerms.size());

  std::cout << "[PASS] test_idempotence_three_levels\n";
}

// ============================================================
// 12. DETERMINISTIC RANDOMIZED TESTING
// ============================================================
void test_deterministic_randomized_models() {
  std::mt19937 gen(1337); // fixed seed for determinism
  std::uniform_int_distribution<int> varCountDist(1, 5);
  std::uniform_int_distribution<int> conCountDist(0, 5);
  std::uniform_real_distribution<double> boundDist(-10.0, 10.0);
  std::uniform_real_distribution<double> coeffDist(-5.0, 5.0);
  std::uniform_int_distribution<int> typeDist(0, 2);
  std::uniform_int_distribution<int> coinDist(0, 1);

  presolve::Presolver presolver;
  constexpr int NUM_RANDOM_MODELS = 30;

  for (int trial = 0; trial < NUM_RANDOM_MODELS; ++trial) {
    model::Model m;
    m.name = "rand_" + std::to_string(trial);

    const int nVars = varCountDist(gen);
    const int nCons = conCountDist(gen);

    for (int j = 0; j < nVars; ++j) {
      double lb = boundDist(gen);
      double ub = boundDist(gen);
      if (lb > ub) std::swap(lb, ub);

      const int t = typeDist(gen);
      model::VariableType vtype = model::VariableType::Continuous;
      if (t == 1) {
        vtype = model::VariableType::Integer;
        lb = std::floor(lb);
        ub = std::ceil(ub);
      } else if (t == 2) {
        vtype = model::VariableType::Binary;
        lb = 0.0;
        ub = 1.0;
      }

      // Small chance of making it fixed
      if (coinDist(gen) == 1 && coinDist(gen) == 1) {
        ub = lb;
      }

      m.variables.push_back(makeVar("x" + std::to_string(j), lb, ub, vtype));
    }

    // Objective
    m.objective.offset = boundDist(gen);
    for (int j = 0; j < nVars; ++j) {
      if (coinDist(gen) == 1) {
        m.objective.linearTerms.push_back({j, coeffDist(gen)});
      }
    }
    // Quadratic terms
    for (int j1 = 0; j1 < nVars; ++j1) {
      for (int j2 = j1; j2 < nVars; ++j2) {
        if (coinDist(gen) == 1 && coinDist(gen) == 1) {
          m.objective.quadraticTerms.push_back({j1, j2, coeffDist(gen)});
        }
      }
    }

    // Constraints
    for (int i = 0; i < nCons; ++i) {
      double lb = boundDist(gen);
      double ub = boundDist(gen);
      if (lb > ub) std::swap(lb, ub);

      // Random infinite bound
      if (coinDist(gen) == 1 && coinDist(gen) == 1) lb = -INF;
      if (coinDist(gen) == 1 && coinDist(gen) == 1) ub = INF;

      auto c = makeCon("c" + std::to_string(i), lb, ub);
      for (int j = 0; j < nVars; ++j) {
        if (coinDist(gen) == 1) {
          c.linearTerms.push_back({j, coeffDist(gen)});
        }
      }
      m.constraints.push_back(c);
    }

    // 1. Validate original
    assert(m.validate());

    // 2. Presolve
    auto res1 = presolver.run(m);

    if (!res1.infeasible) {
      // 3. Validate presolved model
      assert(res1.model.validate());
      assert(res1.converged);

      // 4. Test idempotence
      auto res2 = presolver.run(res1.model);
      assert(!res2.infeasible);
      assert(res2.converged);
      assert(res2.transformations.empty());
      assert(res2.model.variables.size() == res1.model.variables.size());
      assert(res2.model.constraints.size() == res1.model.constraints.size());
      assert(approx(res2.model.objective.offset, res1.model.objective.offset));
    }
  }

  std::cout << "[PASS] test_deterministic_randomized_models (" << NUM_RANDOM_MODELS << " random models verified)\n";
}

} // namespace

int main() {
  std::cout << "Running Adversarial Mathematical Presolve Tests...\n";
  test_empty_and_degenerate_models();
  test_bound_edge_cases();
  test_integer_binary_edge_cases();
  test_singleton_sign_tests();
  test_bound_tightening_adversarial_cascade();
  test_parallel_constraints_all_cases();
  test_redundant_constraints_comprehensive();
  test_qp_adversarial_exhaustive();
  test_index_shift_attacks();
  test_transformation_metadata_attacks();
  test_idempotence_three_levels();
  test_deterministic_randomized_models();

  std::cout << "All Adversarial Presolve tests passed successfully!\n";
  return 0;
}
