#include "presolve/presolver.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <utility>
#include <vector>

namespace presolve {

namespace {

constexpr double EPS = 1e-9;

bool approximatelyEqual(double a, double b) {
  if (std::isinf(a) || std::isinf(b)) {
    return a == b;
  }
  return std::abs(a - b) <= EPS;
}

bool isFinite(double value) { return std::isfinite(value); }

double roundLowerBound(double bound, model::VariableType type) {
  if (!std::isfinite(bound)) return bound;
  if (type == model::VariableType::Integer || type == model::VariableType::Binary) {
    return std::ceil(bound - EPS);
  }
  return bound;
}

double roundUpperBound(double bound, model::VariableType type) {
  if (!std::isfinite(bound)) return bound;
  if (type == model::VariableType::Integer || type == model::VariableType::Binary) {
    return std::floor(bound + EPS);
  }
  return bound;
}

std::map<int, double> buildCoefficientMap(const model::Constraint &constraint) {
  std::map<int, double> coefficients;
  for (const auto &term : constraint.linearTerms) {
    if (std::abs(term.value) > EPS) {
      coefficients[term.variableIndex] += term.value;
    }
  }
  for (auto it = coefficients.begin(); it != coefficients.end();) {
    if (std::abs(it->second) <= EPS) {
      it = coefficients.erase(it);
    } else {
      ++it;
    }
  }
  return coefficients;
}

} // namespace

// ============================================================
// VARIABLE REMOVAL
// ============================================================

void Presolver::removeVariable(model::Model &model, std::size_t variableIndex,
                               std::vector<std::size_t> &currentToOrigVar) {
  if (variableIndex >= model.variables.size()) {
    return;
  }

  const int target = static_cast<int>(variableIndex);

  // 1. Remove from model variables and tracking vector
  model.variables.erase(model.variables.begin() + variableIndex);
  if (variableIndex < currentToOrigVar.size()) {
    currentToOrigVar.erase(currentToOrigVar.begin() + variableIndex);
  }

  // 2. Remove linear objective terms involving target and decrement indices > target
  model.objective.linearTerms.erase(
      std::remove_if(model.objective.linearTerms.begin(),
                     model.objective.linearTerms.end(),
                     [target](const model::LinearTerm &term) {
                       return term.variableIndex == target;
                     }),
      model.objective.linearTerms.end());

  for (auto &term : model.objective.linearTerms) {
    if (term.variableIndex > target) {
      --term.variableIndex;
    }
  }

  // 3. Remove quadratic objective terms involving target and decrement indices > target
  model.objective.quadraticTerms.erase(
      std::remove_if(model.objective.quadraticTerms.begin(),
                     model.objective.quadraticTerms.end(),
                     [target](const model::QuadraticTerm &term) {
                       return term.variableIndex1 == target ||
                              term.variableIndex2 == target;
                     }),
      model.objective.quadraticTerms.end());

  for (auto &term : model.objective.quadraticTerms) {
    if (term.variableIndex1 > target) {
      --term.variableIndex1;
    }
    if (term.variableIndex2 > target) {
      --term.variableIndex2;
    }
  }

  // 4. Remove linear constraint terms involving target and decrement indices > target
  for (auto &constraint : model.constraints) {
    constraint.linearTerms.erase(
        std::remove_if(constraint.linearTerms.begin(),
                       constraint.linearTerms.end(),
                       [target](const model::LinearTerm &term) {
                         return term.variableIndex == target;
                       }),
        constraint.linearTerms.end());

    for (auto &term : constraint.linearTerms) {
      if (term.variableIndex > target) {
        --term.variableIndex;
      }
    }
  }
}

// ============================================================
// ZERO COEFFICIENTS
// ============================================================

bool Presolver::removeZeroCoefficients(model::Model &model) {
  bool changed = false;

  const auto oldLinearTerms = model.objective.linearTerms.size();
  model.objective.linearTerms.erase(
      std::remove_if(model.objective.linearTerms.begin(),
                     model.objective.linearTerms.end(),
                     [](const model::LinearTerm &term) {
                       return std::abs(term.value) <= EPS;
                     }),
      model.objective.linearTerms.end());
  if (model.objective.linearTerms.size() != oldLinearTerms) {
    changed = true;
  }

  const auto oldQuadraticTerms = model.objective.quadraticTerms.size();
  model.objective.quadraticTerms.erase(
      std::remove_if(model.objective.quadraticTerms.begin(),
                     model.objective.quadraticTerms.end(),
                     [](const model::QuadraticTerm &term) {
                       return std::abs(term.value) <= EPS;
                     }),
      model.objective.quadraticTerms.end());
  if (model.objective.quadraticTerms.size() != oldQuadraticTerms) {
    changed = true;
  }

  for (auto &constraint : model.constraints) {
    const auto oldConTerms = constraint.linearTerms.size();
    constraint.linearTerms.erase(
        std::remove_if(constraint.linearTerms.begin(),
                       constraint.linearTerms.end(),
                       [](const model::LinearTerm &term) {
                         return std::abs(term.value) <= EPS;
                       }),
        constraint.linearTerms.end());
    if (constraint.linearTerms.size() != oldConTerms) {
      changed = true;
    }
  }

  return changed;
}

// ============================================================
// FIX VARIABLE
// ============================================================

bool Presolver::fixVariable(model::Model &model, std::size_t variableIndex,
                            double value, PresolveResult &result,
                            std::vector<std::size_t> &currentToOrigVar) {
  if (variableIndex >= model.variables.size()) {
    return false;
  }

  const int target = static_cast<int>(variableIndex);
  const std::size_t origVarIndex = currentToOrigVar[variableIndex];
  const std::string varName = model.variables[variableIndex].name;
  const model::VariableType varType = model.variables[variableIndex].type;

  FixedVariableRecord rec;
  rec.originalIndex = origVarIndex;
  rec.name = varName;
  rec.fixedValue = value;
  rec.type = varType;

  // 1. Linear objective contribution: c_k * x_k -> c_k * value added to offset
  for (auto it = model.objective.linearTerms.begin();
       it != model.objective.linearTerms.end();) {
    if (it->variableIndex == target) {
      const double contribution = it->value * value;
      rec.linearObjectiveContribution += contribution;
      model.objective.offset += contribution;
      it = model.objective.linearTerms.erase(it);
    } else {
      ++it;
    }
  }

  // 2. Quadratic objective contribution:
  //    Diagonal: q_kk * x_k^2 -> q_kk * value^2 added to offset
  //    Cross terms: q_kj * x_k * x_j -> (q_kj * value) * x_j added to linear terms
  std::map<int, double> linearContributions;
  for (auto it = model.objective.quadraticTerms.begin();
       it != model.objective.quadraticTerms.end();) {
    if (it->variableIndex1 == target && it->variableIndex2 == target) {
      const double diagContrib = it->value * value * value;
      rec.quadraticDiagonalContribution += diagContrib;
      model.objective.offset += diagContrib;
      it = model.objective.quadraticTerms.erase(it);
    } else if (it->variableIndex1 == target) {
      const int otherCurrentIdx = it->variableIndex2;
      const std::size_t otherOrigIdx = currentToOrigVar[static_cast<std::size_t>(otherCurrentIdx)];
      const double crossContrib = it->value * value;
      rec.quadraticCrossContributions[otherOrigIdx] += crossContrib;
      linearContributions[otherCurrentIdx] += crossContrib;
      it = model.objective.quadraticTerms.erase(it);
    } else if (it->variableIndex2 == target) {
      const int otherCurrentIdx = it->variableIndex1;
      const std::size_t otherOrigIdx = currentToOrigVar[static_cast<std::size_t>(otherCurrentIdx)];
      const double crossContrib = it->value * value;
      rec.quadraticCrossContributions[otherOrigIdx] += crossContrib;
      linearContributions[otherCurrentIdx] += crossContrib;
      it = model.objective.quadraticTerms.erase(it);
    } else {
      ++it;
    }
  }

  for (const auto &[varIdx, coeff] : linearContributions) {
    if (std::abs(coeff) <= EPS) continue;
    bool found = false;
    for (auto &lt : model.objective.linearTerms) {
      if (lt.variableIndex == varIdx) {
        lt.value += coeff;
        found = true;
        break;
      }
    }
    if (!found) {
      model.objective.linearTerms.push_back({varIdx, coeff});
    }
  }

  // 3. Constraint contributions:
  //    L_i <- L_i - a_ik * value
  //    U_i <- U_i - a_ik * value
  for (auto &constraint : model.constraints) {
    for (auto it = constraint.linearTerms.begin();
         it != constraint.linearTerms.end();) {
      if (it->variableIndex == target) {
        const double contribution = it->value * value;
        if (isFinite(constraint.lowerBound)) {
          constraint.lowerBound -= contribution;
        }
        if (isFinite(constraint.upperBound)) {
          constraint.upperBound -= contribution;
        }
        it = constraint.linearTerms.erase(it);
      } else {
        ++it;
      }
    }

    // Infeasibility check: lower > upper
    if (constraint.lowerBound > constraint.upperBound + EPS) {
      result.infeasible = true;
      return false;
    }

    // Infeasibility check: constraint emptied and 0 not in [lower, upper]
    if (constraint.linearTerms.empty()) {
      if (constraint.lowerBound > EPS || constraint.upperBound < -EPS) {
        result.infeasible = true;
        return false;
      }
    }
  }

  // Record transformation and structured metadata
  Transformation transformation;
  transformation.type = TransformationType::FixVariable;
  transformation.index = variableIndex;
  transformation.originalVariableIndex = origVarIndex;
  transformation.entityName = varName;
  transformation.oldValue = value;
  transformation.newValue = value;
  transformation.offsetChange = rec.linearObjectiveContribution + rec.quadraticDiagonalContribution;
  transformation.reason = "Variable is fixed by equal lower and upper bounds";
  result.transformations.push_back(std::move(transformation));

  result.postsolve.fixedVariables.push_back(std::move(rec));

  // 4. Remove the variable from the model and shift remaining indices
  removeVariable(model, variableIndex, currentToOrigVar);
  return true;
}

// ============================================================
// COEFFICIENT LOOKUP
// ============================================================

double Presolver::coefficientForVariable(const model::Constraint &constraint,
                                         std::size_t variableIndex) const {
  for (const auto &term : constraint.linearTerms) {
    if (term.variableIndex == static_cast<int>(variableIndex)) {
      return term.value;
    }
  }
  return 0.0;
}

// ============================================================
// MINIMUM ACTIVITY
// ============================================================

double Presolver::minConstraintActivity(const model::Constraint &constraint,
                                        const model::Model &model) const {
  double result = 0.0;
  for (const auto &term : constraint.linearTerms) {
    if (term.variableIndex < 0 ||
        static_cast<std::size_t>(term.variableIndex) >= model.variables.size()) {
      continue;
    }
    const auto &variable = model.variables[static_cast<std::size_t>(term.variableIndex)];
    if (term.value > 0.0) {
      if (!isFinite(variable.lowerBound)) {
        return -std::numeric_limits<double>::infinity();
      }
      result += term.value * variable.lowerBound;
    } else if (term.value < 0.0) {
      if (!isFinite(variable.upperBound)) {
        return -std::numeric_limits<double>::infinity();
      }
      result += term.value * variable.upperBound;
    }
  }
  return result;
}

// ============================================================
// MAXIMUM ACTIVITY
// ============================================================

double Presolver::maxConstraintActivity(const model::Constraint &constraint,
                                        const model::Model &model) const {
  double result = 0.0;
  for (const auto &term : constraint.linearTerms) {
    if (term.variableIndex < 0 ||
        static_cast<std::size_t>(term.variableIndex) >= model.variables.size()) {
      continue;
    }
    const auto &variable = model.variables[static_cast<std::size_t>(term.variableIndex)];
    if (term.value > 0.0) {
      if (!isFinite(variable.upperBound)) {
        return std::numeric_limits<double>::infinity();
      }
      result += term.value * variable.upperBound;
    } else if (term.value < 0.0) {
      if (!isFinite(variable.lowerBound)) {
        return std::numeric_limits<double>::infinity();
      }
      result += term.value * variable.lowerBound;
    }
  }
  return result;
}

// ============================================================
// SINGLETON ROWS
// ============================================================

bool Presolver::processSingletonRow(model::Model &model,
                                    std::size_t constraintIndex,
                                    PresolveResult &result,
                                    const std::vector<std::size_t> &currentToOrigVar,
                                    std::vector<std::size_t> &currentToOrigConstraint) {
  if (constraintIndex >= model.constraints.size()) {
    return false;
  }

  const model::Constraint constraint = model.constraints[constraintIndex];
  if (constraint.linearTerms.size() != 1) {
    return false;
  }

  const model::LinearTerm term = constraint.linearTerms.front();
  if (term.variableIndex < 0 ||
      static_cast<std::size_t>(term.variableIndex) >= model.variables.size()) {
    return false;
  }

  const std::size_t variableIndex = static_cast<std::size_t>(term.variableIndex);
  const double coefficient = term.value;
  if (std::abs(coefficient) <= EPS) {
    return false;
  }

  const std::size_t origConIndex = currentToOrigConstraint[constraintIndex];
  const std::size_t origVarIndex = currentToOrigVar[variableIndex];
  const std::string constraintName = constraint.name;
  const std::string varName = model.variables[variableIndex].name;

  auto &variable = model.variables[variableIndex];

  double candLower = -std::numeric_limits<double>::infinity();
  double candUpper = std::numeric_limits<double>::infinity();

  if (coefficient > 0.0) {
    if (isFinite(constraint.lowerBound)) {
      candLower = constraint.lowerBound / coefficient;
    }
    if (isFinite(constraint.upperBound)) {
      candUpper = constraint.upperBound / coefficient;
    }
  } else {
    if (isFinite(constraint.upperBound)) {
      candLower = constraint.upperBound / coefficient;
    }
    if (isFinite(constraint.lowerBound)) {
      candUpper = constraint.lowerBound / coefficient;
    }
  }

  // Integer / Binary bound rounding
  candLower = roundLowerBound(candLower, variable.type);
  candUpper = roundUpperBound(candUpper, variable.type);

  if (candLower > candUpper + EPS) {
    result.infeasible = true;
    return false;
  }

  double newLower = std::max(variable.lowerBound, candLower);
  double newUpper = std::min(variable.upperBound, candUpper);

  if (variable.type == model::VariableType::Binary) {
    newLower = std::max(0.0, newLower);
    newUpper = std::min(1.0, newUpper);
  }

  if (newLower > newUpper + EPS) {
    result.infeasible = true;
    return false;
  }

  if (variable.type == model::VariableType::Integer ||
      variable.type == model::VariableType::Binary) {
    if (std::isfinite(newLower) && std::isfinite(newUpper)) {
      if (std::ceil(newLower - EPS) > std::floor(newUpper + EPS)) {
        result.infeasible = true;
        return false;
      }
    }
  }

  const bool lowerChanged = !approximatelyEqual(variable.lowerBound, newLower);
  const bool upperChanged = !approximatelyEqual(variable.upperBound, newUpper);

  if (lowerChanged) {
    Transformation transformation;
    transformation.type = TransformationType::TightenLowerBound;
    transformation.index = variableIndex;
    transformation.originalVariableIndex = origVarIndex;
    transformation.originalConstraintIndex = origConIndex;
    transformation.entityName = varName;
    transformation.oldValue = variable.lowerBound;
    transformation.newValue = newLower;
    transformation.reason = "Singleton row tightened variable lower bound";
    result.transformations.push_back(std::move(transformation));
    variable.lowerBound = newLower;
  }

  if (upperChanged) {
    Transformation transformation;
    transformation.type = TransformationType::TightenUpperBound;
    transformation.index = variableIndex;
    transformation.originalVariableIndex = origVarIndex;
    transformation.originalConstraintIndex = origConIndex;
    transformation.entityName = varName;
    transformation.oldValue = variable.upperBound;
    transformation.newValue = newUpper;
    transformation.reason = "Singleton row tightened variable upper bound";
    result.transformations.push_back(std::move(transformation));
    variable.upperBound = newUpper;
  }

  // Record structured postsolve metadata for constraint removal
  RemovedConstraintRecord rec;
  rec.originalIndex = origConIndex;
  rec.name = constraintName;
  rec.lowerBound = constraint.lowerBound;
  rec.upperBound = constraint.upperBound;
  rec.reason = "Singleton row converted to variable bounds";
  rec.wasSingleton = true;
  rec.singletonOriginalVarIndex = origVarIndex;
  rec.singletonCoefficient = coefficient;
  result.postsolve.removedConstraints.push_back(std::move(rec));

  Transformation transformation;
  transformation.type = TransformationType::RemoveConstraint;
  transformation.index = constraintIndex;
  transformation.originalConstraintIndex = origConIndex;
  transformation.originalVariableIndex = origVarIndex;
  transformation.entityName = constraintName;
  transformation.reason = "Singleton row converted to variable bounds";
  result.transformations.push_back(std::move(transformation));

  currentToOrigConstraint.erase(currentToOrigConstraint.begin() + constraintIndex);
  model.constraints.erase(model.constraints.begin() + constraintIndex);
  return true;
}

// ============================================================
// SIMPLE INFEASIBILITY
// ============================================================

bool Presolver::detectSimpleInfeasibility(const model::Model &model) const {
  for (const auto &variable : model.variables) {
    if (std::isnan(variable.lowerBound) || std::isnan(variable.upperBound)) {
      return true;
    }
    if (variable.lowerBound > variable.upperBound + EPS) {
      return true;
    }
    if (variable.type == model::VariableType::Binary) {
      if (variable.lowerBound > 1.0 + EPS || variable.upperBound < -EPS) {
        return true;
      }
    }
    if (variable.type == model::VariableType::Integer ||
        variable.type == model::VariableType::Binary) {
      if (std::isfinite(variable.lowerBound) && std::isfinite(variable.upperBound)) {
        if (std::ceil(variable.lowerBound - EPS) > std::floor(variable.upperBound + EPS)) {
          return true;
        }
      }
    }
  }

  for (const auto &constraint : model.constraints) {
    if (std::isnan(constraint.lowerBound) || std::isnan(constraint.upperBound)) {
      return true;
    }
    if (constraint.lowerBound > constraint.upperBound + EPS) {
      return true;
    }

    if (constraint.linearTerms.empty()) {
      if (constraint.lowerBound > EPS || constraint.upperBound < -EPS) {
        return true;
      }
      continue;
    }

    const double minActivity = minConstraintActivity(constraint, model);
    const double maxActivity = maxConstraintActivity(constraint, model);

    if (isFinite(constraint.lowerBound) && isFinite(maxActivity) &&
        maxActivity < constraint.lowerBound - EPS) {
      return true;
    }

    if (isFinite(constraint.upperBound) && isFinite(minActivity) &&
        minActivity > constraint.upperBound + EPS) {
      return true;
    }
  }

  return false;
}

// ============================================================
// BOUND TIGHTENING
// ============================================================

bool Presolver::tightenBounds(model::Model &model, PresolveResult &result,
                             const std::vector<std::size_t> &currentToOrigVar,
                             const std::vector<std::size_t> &currentToOrigConstraint) {
  bool changed = false;

  for (std::size_t cIdx = 0; cIdx < model.constraints.size(); ++cIdx) {
    const auto &constraint = model.constraints[cIdx];
    const std::size_t origConIndex = currentToOrigConstraint[cIdx];

    for (const auto &target : constraint.linearTerms) {
      if (target.variableIndex < 0) continue;
      const std::size_t variableIndex = static_cast<std::size_t>(target.variableIndex);
      if (variableIndex >= model.variables.size()) continue;

      const double a = target.value;
      if (std::abs(a) <= EPS) continue;

      const std::size_t origVarIndex = currentToOrigVar[variableIndex];

      double otherMin = 0.0;
      double otherMax = 0.0;
      bool otherMinFinite = true;
      bool otherMaxFinite = true;

      for (const auto &term : constraint.linearTerms) {
        if (term.variableIndex == target.variableIndex) continue;
        if (term.variableIndex < 0 ||
            static_cast<std::size_t>(term.variableIndex) >= model.variables.size()) {
          otherMinFinite = false;
          otherMaxFinite = false;
          continue;
        }

        const auto &otherVar = model.variables[static_cast<std::size_t>(term.variableIndex)];
        if (term.value > 0.0) {
          if (isFinite(otherVar.lowerBound)) {
            otherMin += term.value * otherVar.lowerBound;
          } else {
            otherMinFinite = false;
          }
          if (isFinite(otherVar.upperBound)) {
            otherMax += term.value * otherVar.upperBound;
          } else {
            otherMaxFinite = false;
          }
        } else if (term.value < 0.0) {
          if (isFinite(otherVar.upperBound)) {
            otherMin += term.value * otherVar.upperBound;
          } else {
            otherMinFinite = false;
          }
          if (isFinite(otherVar.lowerBound)) {
            otherMax += term.value * otherVar.lowerBound;
          } else {
            otherMaxFinite = false;
          }
        }
      }

      auto &variable = model.variables[variableIndex];

      // Lower constraint bound: a * x_k >= L - otherMax
      if (isFinite(constraint.lowerBound) && otherMaxFinite) {
        double candidate = (constraint.lowerBound - otherMax) / a;
        if (a > 0.0) {
          candidate = roundLowerBound(candidate, variable.type);
          if (candidate > variable.upperBound + EPS) {
            result.infeasible = true;
            return false;
          }
          if (candidate > variable.lowerBound + EPS) {
            Transformation transformation;
            transformation.type = TransformationType::TightenLowerBound;
            transformation.index = variableIndex;
            transformation.originalVariableIndex = origVarIndex;
            transformation.originalConstraintIndex = origConIndex;
            transformation.entityName = variable.name;
            transformation.oldValue = variable.lowerBound;
            transformation.newValue = candidate;
            transformation.reason = "Constraint tightened variable lower bound";
            result.transformations.push_back(std::move(transformation));
            variable.lowerBound = candidate;
            changed = true;
          }
        } else {
          candidate = roundUpperBound(candidate, variable.type);
          if (candidate < variable.lowerBound - EPS) {
            result.infeasible = true;
            return false;
          }
          if (candidate < variable.upperBound - EPS) {
            Transformation transformation;
            transformation.type = TransformationType::TightenUpperBound;
            transformation.index = variableIndex;
            transformation.originalVariableIndex = origVarIndex;
            transformation.originalConstraintIndex = origConIndex;
            transformation.entityName = variable.name;
            transformation.oldValue = variable.upperBound;
            transformation.newValue = candidate;
            transformation.reason = "Constraint tightened variable upper bound";
            result.transformations.push_back(std::move(transformation));
            variable.upperBound = candidate;
            changed = true;
          }
        }
      }

      // Upper constraint bound: a * x_k <= U - otherMin
      if (isFinite(constraint.upperBound) && otherMinFinite) {
        double candidate = (constraint.upperBound - otherMin) / a;
        if (a > 0.0) {
          candidate = roundUpperBound(candidate, variable.type);
          if (candidate < variable.lowerBound - EPS) {
            result.infeasible = true;
            return false;
          }
          if (candidate < variable.upperBound - EPS) {
            Transformation transformation;
            transformation.type = TransformationType::TightenUpperBound;
            transformation.index = variableIndex;
            transformation.originalVariableIndex = origVarIndex;
            transformation.originalConstraintIndex = origConIndex;
            transformation.entityName = variable.name;
            transformation.oldValue = variable.upperBound;
            transformation.newValue = candidate;
            transformation.reason = "Constraint tightened variable upper bound";
            result.transformations.push_back(std::move(transformation));
            variable.upperBound = candidate;
            changed = true;
          }
        } else {
          candidate = roundLowerBound(candidate, variable.type);
          if (candidate > variable.upperBound + EPS) {
            result.infeasible = true;
            return false;
          }
          if (candidate > variable.lowerBound + EPS) {
            Transformation transformation;
            transformation.type = TransformationType::TightenLowerBound;
            transformation.index = variableIndex;
            transformation.originalVariableIndex = origVarIndex;
            transformation.originalConstraintIndex = origConIndex;
            transformation.entityName = variable.name;
            transformation.oldValue = variable.lowerBound;
            transformation.newValue = candidate;
            transformation.reason = "Constraint tightened variable lower bound";
            result.transformations.push_back(std::move(transformation));
            variable.lowerBound = candidate;
            changed = true;
          }
        }
      }

      if (variable.type == model::VariableType::Binary) {
        variable.lowerBound = std::max(0.0, variable.lowerBound);
        variable.upperBound = std::min(1.0, variable.upperBound);
      }
      if (variable.lowerBound > variable.upperBound + EPS) {
        result.infeasible = true;
        return false;
      }
      if (variable.type == model::VariableType::Integer ||
          variable.type == model::VariableType::Binary) {
        if (std::isfinite(variable.lowerBound) && std::isfinite(variable.upperBound)) {
          if (std::ceil(variable.lowerBound - EPS) > std::floor(variable.upperBound + EPS)) {
            result.infeasible = true;
            return false;
          }
        }
      }
    }
  }

  return changed;
}

// ============================================================
// REDUNDANT CONSTRAINTS
// ============================================================

bool Presolver::removeRedundantConstraints(model::Model &model,
                                           PresolveResult &result,
                                           std::vector<std::size_t> &currentToOrigConstraint) {
  bool changed = false;

  for (std::size_t i = model.constraints.size(); i-- > 0;) {
    const auto &constraint = model.constraints[i];

    const double minActivity = minConstraintActivity(constraint, model);
    const double maxActivity = maxConstraintActivity(constraint, model);

    // Infeasibility: activity range cannot intersect [lowerBound, upperBound]
    if (isFinite(constraint.lowerBound) && isFinite(maxActivity) &&
        maxActivity < constraint.lowerBound - EPS) {
      result.infeasible = true;
      return false;
    }

    if (isFinite(constraint.upperBound) && isFinite(minActivity) &&
        minActivity > constraint.upperBound + EPS) {
      result.infeasible = true;
      return false;
    }

    bool redundant = true;

    if (isFinite(constraint.lowerBound)) {
      if (!isFinite(minActivity) || minActivity < constraint.lowerBound - EPS) {
        redundant = false;
      }
    }

    if (isFinite(constraint.upperBound)) {
      if (!isFinite(maxActivity) || maxActivity > constraint.upperBound + EPS) {
        redundant = false;
      }
    }

    if (!redundant) {
      continue;
    }

    const std::size_t origConIndex = currentToOrigConstraint[i];
    const std::string constraintName = constraint.name;

    RemovedConstraintRecord rec;
    rec.originalIndex = origConIndex;
    rec.name = constraintName;
    rec.lowerBound = constraint.lowerBound;
    rec.upperBound = constraint.upperBound;
    rec.reason = "Constraint is redundant given variable bounds";
    result.postsolve.removedConstraints.push_back(std::move(rec));

    Transformation transformation;
    transformation.type = TransformationType::RemoveConstraint;
    transformation.index = i;
    transformation.originalConstraintIndex = origConIndex;
    transformation.entityName = constraintName;
    transformation.reason = "Constraint is redundant given variable bounds";
    result.transformations.push_back(std::move(transformation));

    currentToOrigConstraint.erase(currentToOrigConstraint.begin() + i);
    model.constraints.erase(model.constraints.begin() + i);
    changed = true;
  }

  return changed;
}

// ============================================================
// DUPLICATE & PARALLEL ROWS
// ============================================================

bool Presolver::constraintsEquivalent(const model::Constraint &a,
                                      const model::Constraint &b) const {
  const auto coefficientsA = buildCoefficientMap(a);
  const auto coefficientsB = buildCoefficientMap(b);

  if (coefficientsA.size() != coefficientsB.size()) {
    return false;
  }

  for (const auto &[index, valueA] : coefficientsA) {
    auto it = coefficientsB.find(index);
    if (it == coefficientsB.end()) {
      return false;
    }
    if (!approximatelyEqual(valueA, it->second)) {
      return false;
    }
  }

  return approximatelyEqual(a.lowerBound, b.lowerBound) &&
         approximatelyEqual(a.upperBound, b.upperBound);
}

bool Presolver::constraintsParallel(const model::Constraint &a,
                                    const model::Constraint &b) const {
  const auto coefficientsA = buildCoefficientMap(a);
  const auto coefficientsB = buildCoefficientMap(b);

  if (coefficientsA.empty() || coefficientsB.empty()) {
    return false;
  }

  if (coefficientsA.size() != coefficientsB.size()) {
    return false;
  }

  double scale = 0.0;
  bool foundScale = false;

  for (const auto &[index, valueA] : coefficientsA) {
    auto it = coefficientsB.find(index);
    if (it == coefficientsB.end()) {
      return false;
    }

    const double valueB = it->second;
    const double currentScale = valueB / valueA;

    if (!foundScale) {
      scale = currentScale;
      foundScale = true;
    } else if (!approximatelyEqual(currentScale, scale)) {
      return false;
    }
  }

  return foundScale && std::abs(scale) > EPS;
}

bool Presolver::removeDuplicateAndParallelRows(model::Model &model,
                                               PresolveResult &result,
                                               std::vector<std::size_t> &currentToOrigConstraint) {
  bool changed = false;

  for (std::size_t i = 0; i < model.constraints.size(); ++i) {
    for (std::size_t j = model.constraints.size(); j-- > i + 1;) {
      const auto &a = model.constraints[i];
      const auto &b = model.constraints[j];

      if (!constraintsParallel(a, b)) {
        continue;
      }

      // Find scaling factor: b = scale * a
      const auto coefficientsA = buildCoefficientMap(a);
      const auto coefficientsB = buildCoefficientMap(b);

      double scale = 0.0;
      for (const auto &[index, valA] : coefficientsA) {
        auto it = coefficientsB.find(index);
        if (it != coefficientsB.end()) {
          scale = it->second / valA;
          break;
        }
      }

      if (std::abs(scale) <= EPS) {
        continue;
      }

      // Normalized bounds on a^T x implied by constraint b
      double normLowerB = -std::numeric_limits<double>::infinity();
      double normUpperB = std::numeric_limits<double>::infinity();

      if (scale > 0.0) {
        if (isFinite(b.lowerBound)) normLowerB = b.lowerBound / scale;
        if (isFinite(b.upperBound)) normUpperB = b.upperBound / scale;
      } else {
        if (isFinite(b.upperBound)) normLowerB = b.upperBound / scale;
        if (isFinite(b.lowerBound)) normUpperB = b.lowerBound / scale;
      }

      // Check contradiction between [a.lowerBound, a.upperBound] and [normLowerB, normUpperB]
      const double effLower = std::max(a.lowerBound, normLowerB);
      const double effUpper = std::min(a.upperBound, normUpperB);

      if (effLower > effUpper + EPS) {
        // Contradictory parallel constraints!
        result.infeasible = true;
        return false;
      }

      // Check if b is an exact duplicate of a
      if (approximatelyEqual(a.lowerBound, normLowerB) &&
          approximatelyEqual(a.upperBound, normUpperB)) {
        const std::size_t origConIndexJ = currentToOrigConstraint[j];
        const std::size_t origConIndexI = currentToOrigConstraint[i];
        const std::string nameB = b.name;

        RemovedConstraintRecord rec;
        rec.originalIndex = origConIndexJ;
        rec.name = nameB;
        rec.lowerBound = b.lowerBound;
        rec.upperBound = b.upperBound;
        rec.reason = "Duplicate/parallel equivalent constraint removed";
        rec.wasDuplicate = true;
        rec.duplicateOfOriginalIndex = origConIndexI;
        rec.scale = scale;
        result.postsolve.removedConstraints.push_back(std::move(rec));

        Transformation transformation;
        transformation.type = TransformationType::RemoveConstraint;
        transformation.index = j;
        transformation.originalConstraintIndex = origConIndexJ;
        transformation.entityName = nameB;
        transformation.reason = "Duplicate/parallel equivalent constraint removed";
        result.transformations.push_back(std::move(transformation));

        currentToOrigConstraint.erase(currentToOrigConstraint.begin() + j);
        model.constraints.erase(model.constraints.begin() + j);
        changed = true;
      }
    }
  }

  return changed;
}

// ============================================================
// MAIN PRESOLVE PIPELINE
// ============================================================

PresolveResult Presolver::run(const model::Model &input) {
  PresolveResult result;
  result.model = input;
  result.originalVariables = input.variables.size();
  result.originalConstraints = input.constraints.size();
  result.postsolve.initialObjectiveOffset = input.objective.offset;

  // Initialize tracking vectors for original indices
  std::vector<std::size_t> currentToOrigVar(input.variables.size());
  for (std::size_t i = 0; i < input.variables.size(); ++i) {
    currentToOrigVar[i] = i;
  }

  std::vector<std::size_t> currentToOrigConstraint(input.constraints.size());
  for (std::size_t i = 0; i < input.constraints.size(); ++i) {
    currentToOrigConstraint[i] = i;
  }

  // Initial check
  if (detectSimpleInfeasibility(result.model)) {
    result.infeasible = true;
    result.presolvedVariables = result.model.variables.size();
    result.presolvedConstraints = result.model.constraints.size();
    result.postsolve.presolvedObjectiveOffset = result.model.objective.offset;
    return result;
  }

  constexpr int MAX_PASSES = 50;
  bool reachedFixedPoint = false;

  for (int pass = 0; pass < MAX_PASSES; ++pass) {
    if (result.infeasible) break;
    bool changed = false;

    // 1. Remove zero coefficients (linear & quadratic & constraints)
    if (removeZeroCoefficients(result.model)) {
      changed = true;
    }

    // 2. Infeasibility check
    if (detectSimpleInfeasibility(result.model)) {
      result.infeasible = true;
      break;
    }

    // 3. Empty rows
    for (std::size_t i = result.model.constraints.size(); i-- > 0;) {
      const auto &constraint = result.model.constraints[i];
      if (constraint.linearTerms.empty()) {
        const bool satisfied = constraint.lowerBound <= EPS && constraint.upperBound >= -EPS;
        const std::size_t origConIndex = currentToOrigConstraint[i];
        const std::string conName = constraint.name;

        Transformation transformation;
        transformation.type = TransformationType::RemoveConstraint;
        transformation.index = i;
        transformation.originalConstraintIndex = origConIndex;
        transformation.entityName = conName;

        if (satisfied) {
          transformation.reason = "Empty row is redundant";
          result.transformations.push_back(std::move(transformation));

          RemovedConstraintRecord rec;
          rec.originalIndex = origConIndex;
          rec.name = conName;
          rec.lowerBound = constraint.lowerBound;
          rec.upperBound = constraint.upperBound;
          rec.reason = "Empty row is redundant";
          result.postsolve.removedConstraints.push_back(std::move(rec));

          currentToOrigConstraint.erase(currentToOrigConstraint.begin() + i);
          result.model.constraints.erase(result.model.constraints.begin() + i);
          changed = true;
        } else {
          result.infeasible = true;
          transformation.reason = "Empty row makes model infeasible";
          result.transformations.push_back(std::move(transformation));
          break;
        }
      }
    }
    if (result.infeasible) break;

    // 4. Fixed variables (lower == upper)
    for (std::size_t i = result.model.variables.size(); i-- > 0;) {
      const auto &variable = result.model.variables[i];
      if (approximatelyEqual(variable.lowerBound, variable.upperBound)) {
        const double fixedValue = variable.lowerBound;

        if (!fixVariable(result.model, i, fixedValue, result, currentToOrigVar)) {
          result.infeasible = true;
          break;
        }
        changed = true;
      }
    }
    if (result.infeasible) break;

    // 5. Singleton rows
    for (std::size_t i = result.model.constraints.size(); i-- > 0;) {
      if (i < result.model.constraints.size() &&
          result.model.constraints[i].linearTerms.size() == 1) {
        if (processSingletonRow(result.model, i, result, currentToOrigVar, currentToOrigConstraint)) {
          changed = true;
        }
        if (result.infeasible) break;
      }
    }
    if (result.infeasible) break;

    // 6. Bound tightening
    if (tightenBounds(result.model, result, currentToOrigVar, currentToOrigConstraint)) {
      changed = true;
    }
    if (result.infeasible) break;

    // 7. Redundant constraints
    if (removeRedundantConstraints(result.model, result, currentToOrigConstraint)) {
      changed = true;
    }
    if (result.infeasible) break;

    // 8. Duplicate / parallel rows
    if (removeDuplicateAndParallelRows(result.model, result, currentToOrigConstraint)) {
      changed = true;
    }
    if (result.infeasible) break;

    if (!changed) {
      reachedFixedPoint = true;
      break;
    }
  }

  if (!result.infeasible && !reachedFixedPoint) {
    result.converged = false;
  }

  if (!result.infeasible && detectSimpleInfeasibility(result.model)) {
    result.infeasible = true;
  }

  result.presolvedVariables = result.model.variables.size();
  result.presolvedConstraints = result.model.constraints.size();
  result.postsolve.presolvedObjectiveOffset = result.model.objective.offset;

  // Finalize bi-directional index mappings
  result.postsolve.presolvedToOriginalVar = currentToOrigVar;
  result.postsolve.originalToPresolvedVar.assign(result.originalVariables, -1);
  for (std::size_t j = 0; j < currentToOrigVar.size(); ++j) {
    result.postsolve.originalToPresolvedVar[currentToOrigVar[j]] = static_cast<int>(j);
  }

  result.postsolve.presolvedToOriginalConstraint = currentToOrigConstraint;
  result.postsolve.originalToPresolvedConstraint.assign(result.originalConstraints, -1);
  for (std::size_t i = 0; i < currentToOrigConstraint.size(); ++i) {
    result.postsolve.originalToPresolvedConstraint[currentToOrigConstraint[i]] = static_cast<int>(i);
  }

  return result;
}

} // namespace presolve