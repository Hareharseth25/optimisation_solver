#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
#include <vector>

#include "model/model.h"
#include "presolve/presolver.h"

namespace {

constexpr double INF = std::numeric_limits<double>::infinity();
constexpr double EPS = 1e-9;

bool approx(double a, double b) { return std::abs(a - b) <= EPS; }

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

// Helper to evaluate objective of a model at a given vector x
double evalObjective(const model::Model &m, const std::vector<double> &x) {
  double val = m.objective.offset;
  for (const auto &lt : m.objective.linearTerms) {
    val += lt.value * x.at(static_cast<size_t>(lt.variableIndex));
  }
  for (const auto &qt : m.objective.quadraticTerms) {
    val += qt.value * x.at(static_cast<size_t>(qt.variableIndex1)) *
           x.at(static_cast<size_t>(qt.variableIndex2));
  }
  return val;
}

// ============================================================
// 1. QUADRATIC OBJECTIVE TESTS
// ============================================================

// Test: fixed variable in diagonal quadratic term q_kk * x_k^2
void test_qp_fixed_diagonal() {
  model::Model m;
  m.name = "qp_fixed_diagonal";
  m.variables.push_back(makeVar("x0", 3.0, 3.0)); // fixed to 3.0
  m.variables.push_back(makeVar("x1", 0.0, 10.0));

  m.objective.offset = 5.0;
  m.objective.linearTerms.push_back({1, 4.0});      // 4 * x1
  m.objective.quadraticTerms.push_back({0, 0, 2.0}); // 2 * x0^2

  presolve::Presolver presolver;
  auto res = presolver.run(m);

  assert(!res.infeasible);
  assert(res.model.variables.size() == 1);
  assert(res.model.variables[0].name == "x1");

  // Offset should be: original offset (5) + q00 * v0^2 = 5 + 2 * (3^2) = 5 + 18 = 23
  assert(approx(res.model.objective.offset, 23.0));
  assert(res.model.objective.linearTerms.size() == 1);
  assert(res.model.objective.linearTerms[0].variableIndex == 0); // now x1 is index 0
  assert(approx(res.model.objective.linearTerms[0].value, 4.0));
  assert(res.model.objective.quadraticTerms.empty());

  // Check equivalence at test points for x1
  for (double x1_val : {0.0, 2.5, 7.0, 10.0}) {
    double orig_val = evalObjective(m, {3.0, x1_val});
    double presolved_val = evalObjective(res.model, {x1_val});
    assert(approx(orig_val, presolved_val));
  }
  std::cout << "[PASS] QP: Fixed variable in diagonal term\n";
}

// Test: fixed variable in cross term q_kj * x_k * x_j
void test_qp_fixed_cross_term() {
  model::Model m;
  m.name = "qp_fixed_cross";
  m.variables.push_back(makeVar("x0", 4.0, 4.0)); // fixed to 4.0
  m.variables.push_back(makeVar("x1", 0.0, 10.0));

  m.objective.offset = 0.0;
  m.objective.linearTerms.push_back({1, 2.0});      // 2 * x1
  m.objective.quadraticTerms.push_back({0, 1, 3.0}); // 3 * x0 * x1

  presolve::Presolver presolver;
  auto res = presolver.run(m);

  assert(!res.infeasible);
  assert(res.model.variables.size() == 1);
  assert(res.model.variables[0].name == "x1");

  // Linear term for x1 should become: 2.0 + 3.0 * 4.0 = 14.0
  assert(res.model.objective.linearTerms.size() == 1);
  assert(approx(res.model.objective.linearTerms[0].value, 14.0));
  assert(approx(res.model.objective.offset, 0.0));
  assert(res.model.objective.quadraticTerms.empty());

  for (double x1_val : {0.0, 1.0, 5.0}) {
    double orig_val = evalObjective(m, {4.0, x1_val});
    double presolved_val = evalObjective(res.model, {x1_val});
    assert(approx(orig_val, presolved_val));
  }
  std::cout << "[PASS] QP: Fixed variable in cross term\n";
}

// Test: fixed variable with both linear and quadratic terms
void test_qp_fixed_both_linear_and_quadratic() {
  model::Model m;
  m.name = "qp_fixed_both";
  m.variables.push_back(makeVar("x0", 2.0, 2.0)); // fixed to 2.0
  m.variables.push_back(makeVar("x1", 0.0, 5.0));

  m.objective.offset = 3.0;
  m.objective.linearTerms.push_back({0, 4.0}); // 4 * x0
  m.objective.linearTerms.push_back({1, 1.5}); // 1.5 * x1
  m.objective.quadraticTerms.push_back({0, 0, 5.0}); // 5 * x0^2
  m.objective.quadraticTerms.push_back({0, 1, 2.0}); // 2 * x0 * x1
  m.objective.quadraticTerms.push_back({1, 1, 1.0}); // 1 * x1^2

  presolve::Presolver presolver;
  auto res = presolver.run(m);

  assert(!res.infeasible);
  assert(res.model.variables.size() == 1);

  // Expected offset: 3 + 4*2 + 5*(2^2) = 3 + 8 + 20 = 31.0
  assert(approx(res.model.objective.offset, 31.0));

  // Expected linear term for x1: 1.5 + 2*2 = 5.5
  assert(res.model.objective.linearTerms.size() == 1);
  assert(approx(res.model.objective.linearTerms[0].value, 5.5));

  // Expected quadratic term: 1 * x1^2 with index 0
  assert(res.model.objective.quadraticTerms.size() == 1);
  assert(res.model.objective.quadraticTerms[0].variableIndex1 == 0);
  assert(res.model.objective.quadraticTerms[0].variableIndex2 == 0);
  assert(approx(res.model.objective.quadraticTerms[0].value, 1.0));

  for (double x1_val : {0.0, 1.0, 2.5, 5.0}) {
    double orig_val = evalObjective(m, {2.0, x1_val});
    double presolved_val = evalObjective(res.model, {x1_val});
    assert(approx(orig_val, presolved_val));
  }
  std::cout << "[PASS] QP: Fixed variable with linear and quadratic terms\n";
}

// Test: variable removal correctly shifts quadratic variable indices
void test_qp_index_shift() {
  model::Model m;
  m.name = "qp_index_shift";
  m.variables.push_back(makeVar("x0", 1.0, 1.0)); // will be fixed and removed
  m.variables.push_back(makeVar("x1", 0.0, 10.0));
  m.variables.push_back(makeVar("x2", 0.0, 10.0));

  // Quadratic cross term between x1 and x2: 7.0 * x1 * x2
  m.objective.quadraticTerms.push_back({1, 2, 7.0});

  presolve::Presolver presolver;
  auto res = presolver.run(m);

  assert(!res.infeasible);
  assert(res.model.variables.size() == 2);
  assert(res.model.variables[0].name == "x1");
  assert(res.model.variables[1].name == "x2");

  // The quadratic term indices must now be 0 and 1, NOT 1 and 2!
  assert(res.model.objective.quadraticTerms.size() == 1);
  assert(res.model.objective.quadraticTerms[0].variableIndex1 == 0);
  assert(res.model.objective.quadraticTerms[0].variableIndex2 == 1);
  assert(approx(res.model.objective.quadraticTerms[0].value, 7.0));

  // Validation must pass
  assert(res.model.validate());

  for (double x1_val : {1.0, 3.0}) {
    for (double x2_val : {2.0, 4.0}) {
      double orig_val = evalObjective(m, {1.0, x1_val, x2_val});
      double presolved_val = evalObjective(res.model, {x1_val, x2_val});
      assert(approx(orig_val, presolved_val));
    }
  }
  std::cout << "[PASS] QP: Index shift on quadratic terms\n";
}

// Test: variable in quadratic term is not removed as empty column
void test_qp_variable_not_empty_column() {
  model::Model m;
  m.name = "qp_not_empty";
  m.variables.push_back(makeVar("x0", 0.0, 10.0)); // unconstrained, no linear term

  m.objective.quadraticTerms.push_back({0, 0, 1.0}); // min x0^2

  presolve::Presolver presolver;
  auto res = presolver.run(m);

  assert(!res.infeasible);
  assert(res.model.variables.size() == 1);
  assert(res.model.variables[0].name == "x0");
  assert(res.model.objective.quadraticTerms.size() == 1);
  std::cout << "[PASS] QP: Variable in quadratic objective preserved\n";
}

// ============================================================
// 2. INTEGER & BINARY BOUND ROUNDING TESTS
// ============================================================

// Continuous vs Integer bound rounding: 2x <= 5
void test_integer_rounding() {
  // Continuous: 2x <= 5 -> x <= 2.5
  {
    model::Model m;
    m.variables.push_back(makeVar("x", 0.0, 100.0, model::VariableType::Continuous));
    auto c = makeCon("c1", -INF, 5.0);
    c.linearTerms.push_back({0, 2.0});
    m.constraints.push_back(c);

    presolve::Presolver p;
    auto res = p.run(m);
    assert(!res.infeasible);
    assert(res.model.variables.size() == 1);
    assert(approx(res.model.variables[0].upperBound, 2.5));
  }

  // Integer: 2x <= 5 -> x <= 2
  {
    model::Model m;
    m.variables.push_back(makeVar("x", 0.0, 100.0, model::VariableType::Integer));
    auto c = makeCon("c1", -INF, 5.0);
    c.linearTerms.push_back({0, 2.0});
    m.constraints.push_back(c);

    presolve::Presolver p;
    auto res = p.run(m);
    assert(!res.infeasible);
    assert(res.model.variables.size() == 1);
    assert(approx(res.model.variables[0].upperBound, 2.0));
  }

  // Integer lower bound: 2x >= 5 -> x >= 3
  {
    model::Model m;
    m.variables.push_back(makeVar("x", 0.0, 100.0, model::VariableType::Integer));
    auto c = makeCon("c1", 5.0, INF);
    c.linearTerms.push_back({0, 2.0});
    m.constraints.push_back(c);

    presolve::Presolver p;
    auto res = p.run(m);
    assert(!res.infeasible);
    assert(res.model.variables.size() == 1);
    assert(approx(res.model.variables[0].lowerBound, 3.0));
  }
  std::cout << "[PASS] Integer: Ceil on lower bound and floor on upper bound\n";
}

// Integer infeasibility after rounding
void test_integer_infeasibility_after_rounding() {
  model::Model m;
  m.name = "integer_infeasible";
  // x in [0, 1] integer
  m.variables.push_back(makeVar("x", 0.0, 1.0, model::VariableType::Integer));
  // 2x >= 3 -> x >= 1.5 -> rounded x >= 2. Since upper=1, infeasible!
  auto c = makeCon("c1", 3.0, INF);
  c.linearTerms.push_back({0, 2.0});
  m.constraints.push_back(c);

  presolve::Presolver p;
  auto res = p.run(m);
  assert(res.infeasible);
  std::cout << "[PASS] Integer: Infeasibility detected after rounding\n";
}

// Binary variable bound tightening and fixing
void test_binary_bounds() {
  // Binary x in [0, 1], 3x >= 1 -> x >= 1/3 -> ceil(1/3) = 1 -> fixed to 1
  {
    model::Model m;
    m.variables.push_back(makeVar("x", 0.0, 1.0, model::VariableType::Binary));
    auto c = makeCon("c1", 1.0, INF);
    c.linearTerms.push_back({0, 3.0});
    m.constraints.push_back(c);

    presolve::Presolver p;
    auto res = p.run(m);
    assert(!res.infeasible);
    // Since x is fixed to 1, it should be eliminated by fixVariable
    assert(res.model.variables.empty());
  }

  // Binary variable with impossible upper bound
  {
    model::Model m;
    m.variables.push_back(makeVar("x", 0.0, 1.0, model::VariableType::Binary));
    // 2x <= -1 -> x <= -0.5 -> infeasible for binary!
    auto c = makeCon("c1", -INF, -1.0);
    c.linearTerms.push_back({0, 2.0});
    m.constraints.push_back(c);

    presolve::Presolver p;
    auto res = p.run(m);
    assert(res.infeasible);
  }
  std::cout << "[PASS] Binary: Bounds and fixing behavior\n";
}

// ============================================================
// 3. INFEASIBILITY DETECTION TESTS
// ============================================================

// Infeasibility when fixing variable creates empty impossible row
void test_infeasibility_empty_row_after_fixing() {
  model::Model m;
  m.name = "empty_row_infeasible";
  m.variables.push_back(makeVar("x0", 3.0, 3.0)); // fixed to 3
  m.variables.push_back(makeVar("x1", 2.0, 2.0)); // fixed to 2

  // x0 + x1 = 10 -> 3 + 2 = 5 != 10 -> empty row with 0 = 5!
  auto c = makeCon("c1", 10.0, 10.0);
  c.linearTerms.push_back({0, 1.0});
  c.linearTerms.push_back({1, 1.0});
  m.constraints.push_back(c);

  presolve::Presolver p;
  auto res = p.run(m);
  assert(res.infeasible);
  std::cout << "[PASS] Infeasibility: Empty row created after fixing variables\n";
}

// Contradictory parallel inequalities
void test_contradictory_parallel_inequalities() {
  model::Model m;
  m.name = "contradictory_parallel";
  m.variables.push_back(makeVar("x0", -INF, INF));
  m.variables.push_back(makeVar("x1", -INF, INF));

  // x0 + x1 <= 5
  auto c1 = makeCon("c1", -INF, 5.0);
  c1.linearTerms.push_back({0, 1.0});
  c1.linearTerms.push_back({1, 1.0});

  // x0 + x1 >= 10
  auto c2 = makeCon("c2", 10.0, INF);
  c2.linearTerms.push_back({0, 1.0});
  c2.linearTerms.push_back({1, 1.0});

  m.constraints.push_back(c1);
  m.constraints.push_back(c2);

  presolve::Presolver p;
  auto res = p.run(m);
  assert(res.infeasible);
  std::cout << "[PASS] Infeasibility: Contradictory parallel inequalities\n";
}

// Contradictory dependent equalities
void test_contradictory_dependent_equalities() {
  model::Model m;
  m.name = "contradictory_equalities";
  m.variables.push_back(makeVar("x0", -INF, INF));
  m.variables.push_back(makeVar("x1", -INF, INF));

  // x0 + x1 = 10
  auto c1 = makeCon("c1", 10.0, 10.0);
  c1.linearTerms.push_back({0, 1.0});
  c1.linearTerms.push_back({1, 1.0});

  // 2*x0 + 2*x1 = 25 (contradicts 2*10 = 20)
  auto c2 = makeCon("c2", 25.0, 25.0);
  c2.linearTerms.push_back({0, 2.0});
  c2.linearTerms.push_back({1, 2.0});

  m.constraints.push_back(c1);
  m.constraints.push_back(c2);

  presolve::Presolver p;
  auto res = p.run(m);
  assert(res.infeasible);
  std::cout << "[PASS] Infeasibility: Contradictory dependent equalities\n";
}

// Invalid integer interval
void test_invalid_integer_interval() {
  model::Model m;
  m.name = "invalid_int_interval";
  m.variables.push_back(makeVar("x", 0.0, 5.0, model::VariableType::Integer));

  // 3x >= 7 -> x >= 7/3 ~ 2.33 -> ceil = 3
  auto c1 = makeCon("c1", 7.0, INF);
  c1.linearTerms.push_back({0, 3.0});

  // 3x <= 8 -> x <= 8/3 ~ 2.67 -> floor = 2
  auto c2 = makeCon("c2", -INF, 8.0);
  c2.linearTerms.push_back({0, 3.0});

  m.constraints.push_back(c1);
  m.constraints.push_back(c2);

  presolve::Presolver p;
  auto res = p.run(m);
  assert(res.infeasible);
  std::cout << "[PASS] Infeasibility: Invalid integer interval (no integer in [2.33, 2.67])\n";
}

// ============================================================
// 4. REDUNDANT SINGLETON CONSTRAINTS
// ============================================================

void test_redundant_singleton_already_implied() {
  // x in [0, 10], constraint x <= 20
  {
    model::Model m;
    m.variables.push_back(makeVar("x", 0.0, 10.0));
    auto c = makeCon("c1", -INF, 20.0);
    c.linearTerms.push_back({0, 1.0});
    m.constraints.push_back(c);

    presolve::Presolver p;
    auto res = p.run(m);
    assert(!res.infeasible);
    assert(res.model.variables.size() == 1);
    assert(approx(res.model.variables[0].lowerBound, 0.0));
    assert(approx(res.model.variables[0].upperBound, 10.0));
    assert(res.model.constraints.empty()); // constraint must be removed!
  }

  // x in [2, 10], constraint x >= 1
  {
    model::Model m;
    m.variables.push_back(makeVar("x", 2.0, 10.0));
    auto c = makeCon("c1", 1.0, INF);
    c.linearTerms.push_back({0, 1.0});
    m.constraints.push_back(c);

    presolve::Presolver p;
    auto res = p.run(m);
    assert(!res.infeasible);
    assert(res.model.variables.size() == 1);
    assert(approx(res.model.variables[0].lowerBound, 2.0));
    assert(approx(res.model.variables[0].upperBound, 10.0));
    assert(res.model.constraints.empty()); // constraint must be removed!
  }
  std::cout << "[PASS] Singleton: Redundant row already implied by variable bounds is removed\n";
}

} // namespace

int main() {
  std::cout << "\n========================================\n";
  std::cout << "PRESOLVE CORRECTNESS TESTS\n";
  std::cout << "========================================\n\n";

  // 1. QP tests
  test_qp_fixed_diagonal();
  test_qp_fixed_cross_term();
  test_qp_fixed_both_linear_and_quadratic();
  test_qp_index_shift();
  test_qp_variable_not_empty_column();

  // 2. Integer/Binary tests
  test_integer_rounding();
  test_integer_infeasibility_after_rounding();
  test_binary_bounds();

  // 3. Infeasibility tests
  test_infeasibility_empty_row_after_fixing();
  test_contradictory_parallel_inequalities();
  test_contradictory_dependent_equalities();
  test_invalid_integer_interval();

  // 4. Redundant singleton tests
  test_redundant_singleton_already_implied();

  std::cout << "\nAll Presolve correctness tests completed successfully!\n";
  return 0;
}
