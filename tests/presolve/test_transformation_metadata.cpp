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

// 1. Fixed variable records original identity and fixed value
void test_fixed_variable_metadata() {
  model::Model m;
  m.name = "fixed_meta";
  m.variables.push_back(makeVar("x0", 0.0, 10.0));
  m.variables.push_back(makeVar("x1", 3.5, 3.5)); // fixed
  m.variables.push_back(makeVar("x2", 0.0, 5.0));

  presolve::Presolver presolver;
  auto res = presolver.run(m);

  assert(!res.infeasible);
  assert(res.model.variables.size() == 2);

  // Check structured metadata
  assert(res.postsolve.fixedVariables.size() == 1);
  const auto &fix = res.postsolve.fixedVariables[0];
  assert(fix.originalIndex == 1);
  assert(fix.name == "x1");
  assert(approx(fix.fixedValue, 3.5));

  // Check index mappings
  assert(res.postsolve.originalToPresolvedVar.size() == 3);
  assert(res.postsolve.originalToPresolvedVar[0] == 0);
  assert(res.postsolve.originalToPresolvedVar[1] == -1); // eliminated
  assert(res.postsolve.originalToPresolvedVar[2] == 1);

  assert(res.postsolve.presolvedToOriginalVar.size() == 2);
  assert(res.postsolve.presolvedToOriginalVar[0] == 0);
  assert(res.postsolve.presolvedToOriginalVar[1] == 2);

  std::cout << "[PASS] Metadata: Fixed variable original identity and value recorded\n";
}

// 2. Variable identity remains valid after earlier variables are removed
void test_variable_identity_after_earlier_removal() {
  model::Model m;
  m.name = "var_identity_shift";
  m.variables.push_back(makeVar("x0", 1.0, 1.0)); // fixed first (orig 0)
  m.variables.push_back(makeVar("x1", 0.0, 10.0)); // survives (orig 1)
  m.variables.push_back(makeVar("x2", 2.0, 2.0)); // fixed second (orig 2)
  m.variables.push_back(makeVar("x3", 0.0, 10.0)); // survives (orig 3)

  presolve::Presolver presolver;
  auto res = presolver.run(m);

  assert(!res.infeasible);
  assert(res.model.variables.size() == 2);
  assert(res.model.variables[0].name == "x1");
  assert(res.model.variables[1].name == "x3");

  // Check that both fixed variables preserved their true original indices
  assert(res.postsolve.fixedVariables.size() == 2);
  bool found_x0 = false;
  bool found_x2 = false;
  for (const auto &fix : res.postsolve.fixedVariables) {
    if (fix.name == "x0") {
      assert(fix.originalIndex == 0);
      assert(approx(fix.fixedValue, 1.0));
      found_x0 = true;
    }
    if (fix.name == "x2") {
      assert(fix.originalIndex == 2);
      assert(approx(fix.fixedValue, 2.0));
      found_x2 = true;
    }
  }
  assert(found_x0 && found_x2);

  // Mappings
  assert(res.postsolve.originalToPresolvedVar[0] == -1);
  assert(res.postsolve.originalToPresolvedVar[1] == 0);
  assert(res.postsolve.originalToPresolvedVar[2] == -1);
  assert(res.postsolve.originalToPresolvedVar[3] == 1);

  assert(res.postsolve.presolvedToOriginalVar[0] == 1);
  assert(res.postsolve.presolvedToOriginalVar[1] == 3);

  std::cout << "[PASS] Metadata: Variable identity remains valid after earlier removals\n";
}

// 3. Constraint identity remains valid after earlier constraints are removed
void test_constraint_identity_after_earlier_removal() {
  model::Model m;
  m.name = "con_identity_shift";
  m.variables.push_back(makeVar("x0", 0.0, 10.0));
  m.variables.push_back(makeVar("x1", 0.0, 10.0));

  // c0: singleton 2 * x0 <= 10 (orig 0, removed as singleton)
  auto c0 = makeCon("c0", -INF, 10.0);
  c0.linearTerms.push_back({0, 2.0});

  // c1: x0 + x1 <= 12 (orig 1, survives since max activity 5+10=15 > 12)
  auto c1 = makeCon("c1", -INF, 12.0);
  c1.linearTerms.push_back({0, 1.0});
  c1.linearTerms.push_back({1, 1.0});

  // c2: duplicate of c1: x0 + x1 <= 12 (orig 2, removed as duplicate of c1)
  auto c2 = makeCon("c2", -INF, 12.0);
  c2.linearTerms.push_back({0, 1.0});
  c2.linearTerms.push_back({1, 1.0});

  m.constraints.push_back(c0);
  m.constraints.push_back(c1);
  m.constraints.push_back(c2);

  presolve::Presolver presolver;
  auto res = presolver.run(m);

  assert(!res.infeasible);
  assert(res.model.constraints.size() == 1);
  assert(res.model.constraints[0].name == "c1");

  // Check removed constraints metadata
  assert(res.postsolve.removedConstraints.size() == 2);
  bool found_c0 = false;
  bool found_c2 = false;
  for (const auto &rc : res.postsolve.removedConstraints) {
    if (rc.name == "c0") {
      assert(rc.originalIndex == 0);
      assert(rc.wasSingleton);
      assert(rc.singletonOriginalVarIndex == 0);
      assert(approx(rc.singletonCoefficient, 2.0));
      found_c0 = true;
    }
    if (rc.name == "c2") {
      assert(rc.originalIndex == 2);
      assert(rc.wasDuplicate);
      assert(rc.duplicateOfOriginalIndex == 1); // c2 was duplicate of c1!
      assert(approx(rc.scale, 1.0));
      found_c2 = true;
    }
  }
  assert(found_c0 && found_c2);

  // Check constraint mappings
  assert(res.postsolve.originalToPresolvedConstraint.size() == 3);
  assert(res.postsolve.originalToPresolvedConstraint[0] == -1);
  assert(res.postsolve.originalToPresolvedConstraint[1] == 0); // c1 survived at index 0
  assert(res.postsolve.originalToPresolvedConstraint[2] == -1);

  assert(res.postsolve.presolvedToOriginalConstraint.size() == 1);
  assert(res.postsolve.presolvedToOriginalConstraint[0] == 1); // presolved 0 was orig 1

  std::cout << "[PASS] Metadata: Constraint identity remains valid after earlier removals\n";
}

// 4. QP fixed-variable transformation records all required information
void test_qp_fixed_variable_metadata() {
  model::Model m;
  m.name = "qp_meta";
  m.variables.push_back(makeVar("x0", 2.0, 2.0)); // fixed to 2.0
  m.variables.push_back(makeVar("x1", 0.0, 10.0));

  m.objective.offset = 10.0;
  m.objective.linearTerms.push_back({0, 3.0});      // 3 * x0
  m.objective.linearTerms.push_back({1, 2.0});      // 2 * x1
  m.objective.quadraticTerms.push_back({0, 0, 4.0}); // 4 * x0^2
  m.objective.quadraticTerms.push_back({0, 1, 5.0}); // 5 * x0 * x1
  m.objective.quadraticTerms.push_back({1, 1, 1.0}); // 1 * x1^2

  presolve::Presolver presolver;
  auto res = presolver.run(m);

  assert(!res.infeasible);
  assert(res.postsolve.fixedVariables.size() == 1);
  const auto &fix = res.postsolve.fixedVariables[0];
  assert(fix.originalIndex == 0);
  assert(fix.name == "x0");
  assert(approx(fix.fixedValue, 2.0));

  // c0 * v = 3.0 * 2.0 = 6.0
  assert(approx(fix.linearObjectiveContribution, 6.0));

  // q00 * v^2 = 4.0 * (2.0^2) = 16.0
  assert(approx(fix.quadraticDiagonalContribution, 16.0));

  // q01 * v = 5.0 * 2.0 = 10.0 for variable orig 1
  assert(fix.quadraticCrossContributions.count(1) == 1);
  assert(approx(fix.quadraticCrossContributions.at(1), 10.0));

  // Offset preservation
  assert(approx(res.postsolve.initialObjectiveOffset, 10.0));
  // Presolved offset: 10 + 6 + 16 = 32.0
  assert(approx(res.postsolve.presolvedObjectiveOffset, 32.0));
  assert(approx(res.model.objective.offset, 32.0));

  std::cout << "[PASS] Metadata: QP fixed-variable transformation records complete details\n";
}

// 5. Objective offset changes preserved across multiple steps
void test_objective_offset_preservation() {
  model::Model m;
  m.name = "offset_preservation";
  m.variables.push_back(makeVar("x0", 3.0, 3.0));
  m.variables.push_back(makeVar("x1", 4.0, 4.0));
  m.variables.push_back(makeVar("x2", 0.0, 10.0));

  m.objective.offset = 1.0;
  m.objective.linearTerms.push_back({0, 2.0}); // 2 * 3 = 6
  m.objective.linearTerms.push_back({1, 5.0}); // 5 * 4 = 20
  m.objective.quadraticTerms.push_back({0, 0, 1.0}); // 1 * 3^2 = 9
  m.objective.quadraticTerms.push_back({1, 1, 2.0}); // 2 * 4^2 = 32

  presolve::Presolver presolver;
  auto res = presolver.run(m);

  assert(!res.infeasible);
  assert(approx(res.postsolve.initialObjectiveOffset, 1.0));
  // Total offset = 1 + 6 + 20 + 9 + 32 = 68.0
  assert(approx(res.postsolve.presolvedObjectiveOffset, 68.0));
  assert(approx(res.model.objective.offset, 68.0));

  std::cout << "[PASS] Metadata: Objective offset changes preserved\n";
}

// 6. Stable original indices in Transformation log
void test_transformation_log_stable_indices() {
  model::Model m;
  m.name = "log_stable";
  m.variables.push_back(makeVar("x0", 1.0, 1.0)); // fixed
  m.variables.push_back(makeVar("x1", 0.0, 10.0));
  m.variables.push_back(makeVar("x2", 5.0, 5.0)); // fixed

  presolve::Presolver presolver;
  auto res = presolver.run(m);

  assert(!res.infeasible);
  bool checked_x2 = false;
  for (const auto &t : res.transformations) {
    if (t.type == presolve::TransformationType::FixVariable && t.entityName == "x2") {
      assert(t.originalVariableIndex == 2);
      assert(approx(t.newValue, 5.0));
      checked_x2 = true;
    }
  }
  assert(checked_x2);
  std::cout << "[PASS] Metadata: Transformation log records stable original indices\n";
}

} // namespace

int main() {
  std::cout << "\n========================================\n";
  std::cout << "PRESOLVE TRANSFORMATION METADATA TESTS\n";
  std::cout << "========================================\n\n";

  test_fixed_variable_metadata();
  test_variable_identity_after_earlier_removal();
  test_constraint_identity_after_earlier_removal();
  test_qp_fixed_variable_metadata();
  test_objective_offset_preservation();
  test_transformation_log_stable_indices();

  std::cout << "\nAll Transformation metadata tests completed successfully!\n";
  return 0;
}
