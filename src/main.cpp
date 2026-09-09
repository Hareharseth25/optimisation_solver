#include "model/model.h"
#include "presolve/presolver.h"
#include <iostream>
#include <limits>

void printResult(const std::string &name,
                 const presolve::PresolveResult &result) {

  std::cout << "\n========================================\n";
  std::cout << name << "\n";
  std::cout << "========================================\n";

  std::cout << "Original variables: " << result.originalVariables << "\n";

  std::cout << "Presolved variables: " << result.presolvedVariables << "\n";

  std::cout << "Original constraints: " << result.originalConstraints << "\n";

  std::cout << "Presolved constraints: " << result.presolvedConstraints << "\n";

  std::cout << "Infeasible: " << std::boolalpha << result.infeasible << "\n";

  std::cout << "Transformations: " << result.transformations.size() << "\n";

  for (const auto &t : result.transformations) {

    std::cout << "  - Index: " << t.index << " | Reason: " << t.reason << "\n";
  }

  std::cout << "Objective offset: " << result.model.objective.offset << "\n";

  std::cout << "Variables:\n";

  for (const auto &v : result.model.variables) {

    std::cout << "  " << v.name << " [" << v.lowerBound << ", " << v.upperBound
              << "]\n";
  }

  std::cout << "Constraints:\n";

  for (const auto &c : result.model.constraints) {

    std::cout << "  " << c.name << " [" << c.lowerBound << ", " << c.upperBound
              << "]"
              << " | terms: " << c.linearTerms.size() << "\n";
  }
}

// ============================================================
// TEST 1 — Zero coefficients
// ============================================================

void testZeroCoefficients() {

  model::Model model;

  model.name = "zero_coeff_test";

  model.variables.push_back({"x", model::VariableType::Continuous, 0.0, 10.0});

  model.variables.push_back({"y", model::VariableType::Continuous, 0.0, 10.0});

  model.objective.linearTerms.push_back({0, 1.0});
  model.objective.linearTerms.push_back({1, 0.0});

  model::Constraint constraint;

  constraint.name = "c1";
  constraint.lowerBound = 5.0;
  constraint.upperBound = std::numeric_limits<double>::infinity();

  constraint.linearTerms.push_back({0, 1.0});
  constraint.linearTerms.push_back({1, 0.0});

  model.constraints.push_back(constraint);

  presolve::Presolver presolver;

  auto result = presolver.run(model);

  printResult("TEST 1 — ZERO COEFFICIENTS", result);
}

// ============================================================
// TEST 2 — Empty row, feasible
// ============================================================

void testEmptyRowFeasible() {

  model::Model model;

  model.name = "empty_row_feasible";

  model::Constraint constraint;

  constraint.name = "empty";
  constraint.lowerBound = -10.0;
  constraint.upperBound = 10.0;

  model.constraints.push_back(constraint);

  presolve::Presolver presolver;

  auto result = presolver.run(model);

  printResult("TEST 2 — EMPTY ROW FEASIBLE", result);
}

// ============================================================
// TEST 3 — Empty row, infeasible
// ============================================================

void testEmptyRowInfeasible() {

  model::Model model;

  model.name = "empty_row_infeasible";

  model::Constraint constraint;

  constraint.name = "empty";
  constraint.lowerBound = 5.0;
  constraint.upperBound = 10.0;

  model.constraints.push_back(constraint);

  presolve::Presolver presolver;

  auto result = presolver.run(model);

  printResult("TEST 3 — EMPTY ROW INFEASIBLE", result);
}

// ============================================================
// TEST 4 — Empty column
// ============================================================

void testEmptyColumn() {

  model::Model model;

  model.name = "empty_column_test";

  model.variables.push_back({"x", model::VariableType::Continuous, 0.0, 10.0});

  model.variables.push_back(
      {"unused", model::VariableType::Continuous, 0.0, 10.0});

  model.objective.linearTerms.push_back({0, 1.0});

  model::Constraint constraint;

  constraint.name = "c1";
  constraint.lowerBound = 5.0;
  constraint.upperBound = std::numeric_limits<double>::infinity();

  constraint.linearTerms.push_back({0, 1.0});

  model.constraints.push_back(constraint);

  presolve::Presolver presolver;

  auto result = presolver.run(model);

  printResult("TEST 4 — EMPTY COLUMN", result);
}

// ============================================================
// TEST 5 — Fixed variable
// ============================================================

void testFixedVariable() {

  model::Model model;

  model.name = "fixed_variable_test";

  model.variables.push_back({"x", model::VariableType::Continuous, 0.0, 100.0});

  model.variables.push_back({"y", model::VariableType::Continuous, 5.0, 5.0});

  model.objective.linearTerms.push_back({0, 1.0});
  model.objective.linearTerms.push_back({1, 2.0});

  model::Constraint constraint;

  constraint.name = "demand";
  constraint.lowerBound = 10.0;
  constraint.upperBound = std::numeric_limits<double>::infinity();

  constraint.linearTerms.push_back({0, 1.0});
  constraint.linearTerms.push_back({1, 1.0});

  model.constraints.push_back(constraint);

  presolve::Presolver presolver;

  auto result = presolver.run(model);

  printResult("TEST 5 — FIXED VARIABLE", result);
}

// ============================================================
// TEST 6 — Singleton row: equality
// ============================================================
//
// Constraint:
//     2x = 10
//
// Expected:
//     x = 5
//     constraint removed
// ============================================================

void testSingletonEquality() {

  model::Model model;

  model.name = "singleton_equality";

  model.variables.push_back({"x", model::VariableType::Continuous, 0.0, 100.0});

  model::Constraint constraint;

  constraint.name = "singleton_eq";
  constraint.lowerBound = 10.0;
  constraint.upperBound = 10.0;

  constraint.linearTerms.push_back({0, 2.0});

  model.constraints.push_back(constraint);

  presolve::Presolver presolver;

  auto result = presolver.run(model);

  printResult("TEST 6 — SINGLETON ROW EQUALITY", result);
}

// ============================================================
// TEST 7 — Singleton row: negative coefficient
// ============================================================
//
// Constraint:
//     -2x >= -10
//
// Therefore:
//     x <= 5
//
// Expected:
//     upper bound becomes 5
// ============================================================

void testSingletonNegativeCoefficient() {

  model::Model model;

  model.name = "singleton_negative";

  model.variables.push_back({"x", model::VariableType::Continuous, 0.0, 100.0});

  model::Constraint constraint;

  constraint.name = "singleton_negative";
  constraint.lowerBound = -10.0;
  constraint.upperBound = std::numeric_limits<double>::infinity();

  constraint.linearTerms.push_back({0, -2.0});

  model.constraints.push_back(constraint);

  presolve::Presolver presolver;

  auto result = presolver.run(model);

  printResult("TEST 7 — SINGLETON NEGATIVE COEFFICIENT", result);
}

// ============================================================
// TEST 8 — Simple infeasibility from variable bounds
// ============================================================
//
// Variable:
//     10 <= x <= 5
//
// Expected:
//     infeasible = true
// ============================================================

void testVariableBoundInfeasibility() {

  model::Model model;

  model.name = "variable_bound_infeasible";

  model.variables.push_back({"x", model::VariableType::Continuous, 10.0, 5.0});

  presolve::Presolver presolver;

  auto result = presolver.run(model);

  printResult("TEST 8 — VARIABLE BOUND INFEASIBILITY", result);
}

// ============================================================
// TEST 9 — Simple infeasibility from constraint + variable bounds
// ============================================================
//
// Variable:
//     0 <= x <= 5
//
// Constraint:
//     x >= 10
//
// Expected:
//     infeasible = true
// ============================================================

void testConstraintBoundInfeasibility() {

  model::Model model;

  model.name = "constraint_bound_infeasible";

  model.variables.push_back({"x", model::VariableType::Continuous, 0.0, 5.0});

  model::Constraint constraint;

  constraint.name = "impossible";
  constraint.lowerBound = 10.0;
  constraint.upperBound = std::numeric_limits<double>::infinity();

  constraint.linearTerms.push_back({0, 1.0});

  model.constraints.push_back(constraint);

  presolve::Presolver presolver;

  auto result = presolver.run(model);

  printResult("TEST 9 — CONSTRAINT INFEASIBILITY", result);
}

// ============================================================
// TEST 10 — Bound tightening
// ============================================================
//
// Variables:
//     0 <= x <= 100
//     0 <= y <= 5
//
// Constraint:
//     x + y >= 10
//
// Since y <= 5:
//
//     x >= 10 - 5
//     x >= 5
//
// Expected:
//     x lower bound becomes 5
// ============================================================

void testBoundTightening() {

  model::Model model;

  model.name = "bound_tightening";

  model.variables.push_back({"x", model::VariableType::Continuous, 0.0, 100.0});

  model.variables.push_back({"y", model::VariableType::Continuous, 0.0, 5.0});

  model::Constraint constraint;

  constraint.name = "demand";
  constraint.lowerBound = 10.0;
  constraint.upperBound = std::numeric_limits<double>::infinity();

  constraint.linearTerms.push_back({0, 1.0});
  constraint.linearTerms.push_back({1, 1.0});

  model.constraints.push_back(constraint);

  presolve::Presolver presolver;

  auto result = presolver.run(model);

  printResult("TEST 10 — BOUND TIGHTENING", result);
}

// ============================================================
// TEST 11 — Implied bound
// ============================================================
//
// Variables:
//     0 <= x <= 100
//     0 <= y <= 10
//
// Constraint:
//     x + y <= 15
//
// Since y >= 0:
//
//     x <= 15
//
// Expected:
//     x upper bound becomes 15
// ============================================================

void testImpliedBound() {

  model::Model model;

  model.name = "implied_bound";

  model.variables.push_back({"x", model::VariableType::Continuous, 0.0, 100.0});

  model.variables.push_back({"y", model::VariableType::Continuous, 0.0, 10.0});

  model::Constraint constraint;

  constraint.name = "capacity";
  constraint.lowerBound = -std::numeric_limits<double>::infinity();
  constraint.upperBound = 15.0;

  constraint.linearTerms.push_back({0, 1.0});
  constraint.linearTerms.push_back({1, 1.0});

  model.constraints.push_back(constraint);

  presolve::Presolver presolver;

  auto result = presolver.run(model);

  printResult("TEST 11 — IMPLIED BOUND", result);
}

// ============================================================
// TEST 12 — Redundant constraint
// ============================================================
//
// Variable:
//     0 <= x <= 5
//
// Constraint:
//     x <= 10
//
// The variable bounds already guarantee the constraint.
//
// Expected:
//     constraint removed
// ============================================================

void testRedundantConstraint() {

  model::Model model;

  model.name = "redundant_constraint";

  model.variables.push_back({"x", model::VariableType::Continuous, 0.0, 5.0});

  model::Constraint constraint;

  constraint.name = "redundant";
  constraint.lowerBound = -std::numeric_limits<double>::infinity();
  constraint.upperBound = 10.0;

  constraint.linearTerms.push_back({0, 1.0});

  model.constraints.push_back(constraint);

  presolve::Presolver presolver;

  auto result = presolver.run(model);

  printResult("TEST 12 — REDUNDANT CONSTRAINT", result);
}

// ============================================================
// TEST 13 — DUPLICATE CONSTRAINT
//
// Two constraints impose exactly the same restriction.
//
// Expected:
//     one constraint removed
// ============================================================

// ============================================================
// TEST 13 — DUPLICATE CONSTRAINT
//
// Two identical multi-variable constraints.
//
// Expected:
//     one duplicate constraint removed
// ============================================================

void testDuplicateConstraint() {

  model::Model model;

  model.name = "duplicate_constraint";

  model.variables.push_back({"x", model::VariableType::Continuous, 0.0, 10.0});

  model.variables.push_back({"y", model::VariableType::Continuous, 0.0, 10.0});

  model::Constraint c1;

  c1.name = "constraint_1";
  c1.lowerBound = 5.0;
  c1.upperBound = 15.0;

  c1.linearTerms.push_back({0, 1.0});
  c1.linearTerms.push_back({1, 1.0});

  model::Constraint c2;

  c2.name = "constraint_2";
  c2.lowerBound = 5.0;
  c2.upperBound = 15.0;

  c2.linearTerms.push_back({0, 1.0});
  c2.linearTerms.push_back({1, 1.0});

  model.constraints.push_back(c1);
  model.constraints.push_back(c2);

  presolve::Presolver presolver;

  auto result = presolver.run(model);

  printResult("TEST 13 — DUPLICATE CONSTRAINT", result);
}

// ============================================================
// TEST 14 — PARALLEL EQUIVALENT CONSTRAINTS
//
// Same constraint direction, same bounds, coefficients scaled.
//
// x + y in [5, 15]
// 2x + 2y in [10, 30]
//
// The second constraint is exactly equivalent to the first.
//
// Expected:
//     one constraint removed
// ============================================================

void testParallelEquivalentConstraints() {

  model::Model model;

  model.name = "parallel_equivalent_constraints";

  model.variables.push_back({"x", model::VariableType::Continuous, 0.0, 10.0});

  model.variables.push_back({"y", model::VariableType::Continuous, 0.0, 10.0});

  model::Constraint c1;

  c1.name = "constraint_1";
  c1.lowerBound = 5.0;
  c1.upperBound = 15.0;

  c1.linearTerms.push_back({0, 1.0});
  c1.linearTerms.push_back({1, 1.0});

  model::Constraint c2;

  c2.name = "constraint_2";
  c2.lowerBound = 10.0;
  c2.upperBound = 30.0;

  c2.linearTerms.push_back({0, 2.0});
  c2.linearTerms.push_back({1, 2.0});

  model.constraints.push_back(c1);
  model.constraints.push_back(c2);

  presolve::Presolver presolver;

  auto result = presolver.run(model);

  printResult("TEST 14 — PARALLEL EQUIVALENT CONSTRAINTS", result);
}

void testDependentEquations() {
  std::cout << "\n========================================\n";
  std::cout << "TEST 15 — DEPENDENT EQUATIONS\n";
  std::cout << "========================================\n";

  model::Model model;

  // -----------------------------
  // Variables
  // -----------------------------

  model::Variable x;
  x.name = "x";
  x.lowerBound = 0.0;
  x.upperBound = 10.0;

  model::Variable y;
  y.name = "y";
  y.lowerBound = 0.0;
  y.upperBound = 10.0;

  model.variables.push_back(x);
  model.variables.push_back(y);

  // -----------------------------
  // First equality:
  //
  // x + y = 10
  // -----------------------------

  model::Constraint equation1;
  equation1.name = "equation_1";
  equation1.lowerBound = 10.0;
  equation1.upperBound = 10.0;

  equation1.linearTerms.push_back({0, 1.0});
  equation1.linearTerms.push_back({1, 1.0});

  model.constraints.push_back(equation1);

  // -----------------------------
  // Second equality:
  //
  // 2x + 2y = 20
  //
  // This is dependent on equation 1.
  // -----------------------------

  model::Constraint equation2;
  equation2.name = "equation_2";
  equation2.lowerBound = 20.0;
  equation2.upperBound = 20.0;

  equation2.linearTerms.push_back({0, 2.0});
  equation2.linearTerms.push_back({1, 2.0});

  model.constraints.push_back(equation2);

  // -----------------------------
  // Run presolver
  // -----------------------------

  presolve::Presolver presolver;

  auto result = presolver.run(model);

  // -----------------------------
  // Print result
  // -----------------------------

  std::cout << "Original variables: " << result.originalVariables << "\n";

  std::cout << "Presolved variables: " << result.presolvedVariables << "\n";

  std::cout << "Original constraints: " << result.originalConstraints << "\n";

  std::cout << "Presolved constraints: " << result.presolvedConstraints << "\n";

  std::cout << "Infeasible: " << (result.infeasible ? "true" : "false") << "\n";

  std::cout << "Transformations: " << result.transformations.size() << "\n";

  for (const auto &t : result.transformations) {
    std::cout << "  - Index: " << t.index << " | Reason: " << t.reason << "\n";
  }

  std::cout << "Objective offset: " << result.model.objective.offset << "\n";

  std::cout << "Variables:\n";

  for (const auto &variable : result.model.variables) {
    std::cout << "  " << variable.name << " [" << variable.lowerBound << ", "
              << variable.upperBound << "]\n";
  }

  std::cout << "Constraints:\n";

  for (const auto &constraint : result.model.constraints) {
    std::cout << "  " << constraint.name << " [" << constraint.lowerBound
              << ", " << constraint.upperBound
              << "] | terms: " << constraint.linearTerms.size() << "\n";
  }
}

// ============================================================
// MAIN
// ============================================================

int main() {
  testZeroCoefficients();

  testEmptyRowFeasible();

  testEmptyRowInfeasible();

  testEmptyColumn();

  testFixedVariable();

  testSingletonEquality();

  testSingletonNegativeCoefficient();

  testVariableBoundInfeasibility();

  testConstraintBoundInfeasibility();

  testBoundTightening();

  testImpliedBound();

  testRedundantConstraint();

  testDuplicateConstraint();

  testParallelEquivalentConstraints();

  testDependentEquations();

  return 0;
}