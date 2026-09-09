#include "model/model.h"

#include <cmath>
#include <string>
#include <unordered_set>

namespace model {

bool Model::validate() const {

  // ------------------------------------------------------------
  // 1. Validate objective sense
  // ------------------------------------------------------------

  switch (objective.sense) {
  case ObjectiveSense::Minimize:
  case ObjectiveSense::Maximize:
    break;

  default:
    return false;
  }

  // ------------------------------------------------------------
  // 2. Validate objective offset
  // ------------------------------------------------------------

  if (!std::isfinite(objective.offset)) {
    return false;
  }

  // ------------------------------------------------------------
  // 3. Validate variables
  // ------------------------------------------------------------

  std::unordered_set<std::string> variableNames;

  for (const auto &variable : variables) {

    // Variable must have a name
    if (variable.name.empty()) {
      return false;
    }

    // Variable names must be unique
    if (!variableNames.insert(variable.name).second) {
      return false;
    }

    // Bounds cannot be NaN
    if (std::isnan(variable.lowerBound) || std::isnan(variable.upperBound)) {
      return false;
    }

    // Lower bound cannot be greater than upper bound
    if (variable.lowerBound > variable.upperBound) {
      return false;
    }

    // Binary variables must stay within [0, 1]
    if (variable.type == VariableType::Binary) {
      if (variable.lowerBound < 0.0 || variable.upperBound > 1.0) {
        return false;
      }
    }
  }

  // ------------------------------------------------------------
  // 4. Validate objective linear terms
  // ------------------------------------------------------------

  for (const auto &term : objective.linearTerms) {

    // Variable index must exist
    if (term.variableIndex < 0 ||
        static_cast<size_t>(term.variableIndex) >= variables.size()) {
      return false;
    }

    // Coefficient cannot be NaN or infinity
    if (!std::isfinite(term.value)) {
      return false;
    }
  }

  // ------------------------------------------------------------
  // 5. Validate objective quadratic terms
  // ------------------------------------------------------------

  for (const auto &term : objective.quadraticTerms) {

    if (term.variableIndex1 < 0 ||
        static_cast<size_t>(term.variableIndex1) >= variables.size()) {
      return false;
    }

    if (term.variableIndex2 < 0 ||
        static_cast<size_t>(term.variableIndex2) >= variables.size()) {
      return false;
    }

    if (!std::isfinite(term.value)) {
      return false;
    }
  }

  // ------------------------------------------------------------
  // 6. Validate constraints
  // ------------------------------------------------------------

  std::unordered_set<std::string> constraintNames;

  for (const auto &constraint : constraints) {

    // Constraint must have a name
    if (constraint.name.empty()) {
      return false;
    }

    // Constraint names must be unique
    if (!constraintNames.insert(constraint.name).second) {
      return false;
    }

    // Bounds cannot be NaN
    if (std::isnan(constraint.lowerBound) ||
        std::isnan(constraint.upperBound)) {
      return false;
    }

    // Lower bound cannot exceed upper bound
    if (constraint.lowerBound > constraint.upperBound) {
      return false;
    }

    // Validate every linear term
    for (const auto &term : constraint.linearTerms) {

      if (term.variableIndex < 0 ||
          static_cast<size_t>(term.variableIndex) >= variables.size()) {
        return false;
      }

      if (!std::isfinite(term.value)) {
        return false;
      }
    }
  }

  // ------------------------------------------------------------
  // Everything is structurally valid
  // ------------------------------------------------------------

  return true;
}

} // namespace model