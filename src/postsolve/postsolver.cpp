#include "postsolve/postsolver.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <unordered_set>

namespace postsolve {

PostsolveResult Postsolver::process(
    const model::Model& originalModel,
    const presolve::PresolveResult& presolveResult,
    const std::vector<double>& presolvedPrimalSolution) {
  PostsolveResult result;

  if (presolveResult.infeasible) {
    result.status = PostsolveStatus::InfeasiblePresolve;
    result.errorMessage = "Presolve marked the problem as infeasible.";
    return result;
  }

  // Validate the presolve metadata/mapping before dereferencing any of it.
  if (!validateMapping(originalModel, presolveResult, presolvedPrimalSolution, result)) {
    return result;
  }

  const auto& meta = presolveResult.postsolve;
  std::size_t nOrigVars = originalModel.variables.size();

  // 1. Initialize full primal solution vector for original variables
  std::vector<double> x(nOrigVars, 0.0);

  // Map presolved variable values back to their original positions
  for (std::size_t pIdx = 0; pIdx < presolvedPrimalSolution.size(); ++pIdx) {
  const std::size_t origIdx = meta.presolvedToOriginalVar[pIdx];
  x[origIdx] = presolvedPrimalSolution[pIdx];
}

  // 2. Map fixed variables back to their fixed values
  for (const auto& fixed : meta.fixedVariables) {
  x[fixed.originalIndex] = fixed.fixedValue;
}

  // 3. Apply transformation log in reverse order (for bound modifications / substitutions)
for (auto it = presolveResult.transformations.rbegin();
     it != presolveResult.transformations.rend(); ++it) {
  const auto& trans = *it;

  switch (trans.type) {
    case presolve::TransformationType::FixVariable:
      // Fixed variables are already restored from
      // PostsolveMetadata::fixedVariables above.
      break;

    case presolve::TransformationType::TightenLowerBound:
    case presolve::TransformationType::TightenUpperBound:
      // Bound tightening changes the presolved model but does not
      // require reconstructing a variable value during postsolve.
      break;

    case presolve::TransformationType::RemoveConstraint:
      // Removed constraints do not change the primal variable vector.
      break;

    case presolve::TransformationType::RemoveVariable:
      // Eliminated variables are restored through
      // PostsolveMetadata.
      break;

    case presolve::TransformationType::SubstituteVariable:
      // The current presolver does not generate general substitution
      // transformations, so no substitution reconstruction is needed.
      break;
  }
}
  // 4. Calculate original objective value
  result.originalObjectiveValue = evaluateObjective(originalModel, x);
  result.primalSolution = x;

  // 5. Validate final primal solution against original model bounds and constraints
  if (!validateSolution(originalModel, x, result)) {
    return result;
  }

  result.status = PostsolveStatus::Success;
  return result;
}

double Postsolver::evaluateObjective(
    const model::Model& model,
    const std::vector<double>& x) const {
  double objVal = model.objective.offset;

  // Linear terms
  for (const auto& term : model.objective.linearTerms) {
    if (term.variableIndex >= 0 && static_cast<std::size_t>(term.variableIndex) < x.size()) {
      objVal += term.value * x[term.variableIndex];
    }
  }

  // Quadratic terms
  for (const auto& term : model.objective.quadraticTerms) {
    if (term.variableIndex1 >= 0 && static_cast<std::size_t>(term.variableIndex1) < x.size() &&
        term.variableIndex2 >= 0 && static_cast<std::size_t>(term.variableIndex2) < x.size()) {
      // Model convention: f(x) = offset + sum(c_i x_i) + sum(q_ij x_i x_j),
      // where QuadraticTerm.value is the DIRECT coefficient of x_i * x_j.
      // There is no implicit 1/2 factor, for diagonal or off-diagonal terms.
      objVal += term.value * x[term.variableIndex1] * x[term.variableIndex2];
    }
  }

  return objVal;
}

bool Postsolver::validateMapping(
    const model::Model& originalModel,
    const presolve::PresolveResult& presolveResult,
    const std::vector<double>& presolvedPrimalSolution,
    PostsolveResult& result) const {
  const auto& meta = presolveResult.postsolve;
  const std::size_t nOrigVars = originalModel.variables.size();
  const std::size_t nOrigCons = originalModel.constraints.size();

  auto invalid = [&](const std::string& message) {
    result.status = PostsolveStatus::InvalidMapping;
    result.errorMessage = message;
    return false;
  };

  // presolvedToOriginalVar.size() must match the presolved variable count.
  if (meta.presolvedToOriginalVar.size() != presolveResult.presolvedVariables) {
    return invalid(
        "Invalid mapping: presolvedToOriginalVar size does not match the presolved variable count.");
  }

  // The presolved solution vector must line up with the presolved variable mapping.
  if (presolvedPrimalSolution.size() != meta.presolvedToOriginalVar.size()) {
    return invalid(
        "Invalid mapping: presolved solution size does not match presolvedToOriginalVar size.");
  }

  // Every original variable index referenced by the mapping must be in range,
  // and no original variable may be claimed by more than one presolved variable.
  // Track coverage across all original variables to ensure every original variable
  // is represented exactly once (either mapped or fixed, never both, never missing).
  std::vector<bool> covered(nOrigVars, false);

  for (std::size_t origIdx : meta.presolvedToOriginalVar) {
    if (origIdx >= nOrigVars) {
      return invalid(
          "Invalid mapping: presolvedToOriginalVar contains an out-of-range original variable index.");
    }
    if (covered[origIdx]) {
      return invalid(
          "Invalid mapping: presolvedToOriginalVar contains a duplicate original variable index.");
    }
    covered[origIdx] = true;
  }

  // Fixed-variable metadata must reference valid original variables, including
  // any bilinear cross-contribution indices that will later be dereferenced.
  std::unordered_set<std::size_t> fixedOrigVars;
  fixedOrigVars.reserve(meta.fixedVariables.size());
  for (const auto& fixed : meta.fixedVariables) {
    if (fixed.originalIndex >= nOrigVars) {
      return invalid(
          "Invalid mapping: fixed-variable record has an out-of-range original variable index.");
    }
    // A variable that is fixed by presolve cannot also survive into the
    // presolved model under the same original index.
    if (covered[fixed.originalIndex]) {
      return invalid(
          "Invalid mapping: fixed-variable original index also appears in presolvedToOriginalVar.");
    }
    if (!fixedOrigVars.insert(fixed.originalIndex).second) {
      return invalid(
          "Invalid mapping: duplicate fixed-variable record for original variable index.");
    }
    covered[fixed.originalIndex] = true;

    for (const auto& [otherOrigIdx, contribution] : fixed.quadraticCrossContributions) {
      (void)contribution;
      if (otherOrigIdx >= nOrigVars) {
        return invalid(
            "Invalid mapping: fixed-variable cross-contribution references an out-of-range "
            "original variable index.");
      }
    }
  }

  // Verify complete original-variable coverage:
  // mapped original variables + fixed variables = every original variable exactly once.
  for (std::size_t i = 0; i < nOrigVars; ++i) {
    if (!covered[i]) {
      return invalid(
          "Invalid mapping: original variable " + std::to_string(i) +
          " is neither mapped nor fixed (incomplete variable coverage).");
    }
  }

  // Removed-constraint metadata must reference valid original constraints/variables.
  for (const auto& removed : meta.removedConstraints) {
    if (removed.originalIndex >= nOrigCons) {
      return invalid(
          "Invalid mapping: removed-constraint record has an out-of-range original constraint index.");
    }
    if (removed.wasSingleton && removed.singletonOriginalVarIndex >= nOrigVars) {
      return invalid(
          "Invalid mapping: removed-constraint singleton record has an out-of-range original "
          "variable index.");
    }
    if (removed.wasDuplicate && removed.duplicateOfOriginalIndex >= nOrigCons) {
      return invalid(
          "Invalid mapping: removed-constraint duplicate record has an out-of-range original "
          "constraint index.");
    }
  }

  return true;
}

bool Postsolver::validateSolution(
    const model::Model& originalModel,
    const std::vector<double>& x,
    PostsolveResult& result) const {
  result.maxBoundResidual = 0.0;
  result.maxConstraintResidual = 0.0;

  // 1. Explicitly reject all non-finite primal values (NaN, +Inf, -Inf)
  // before ordinary bound, integrality, or constraint validation.
  for (std::size_t i = 0; i < originalModel.variables.size(); ++i) {
    double val = x[i];
    if (!std::isfinite(val)) {
      result.status = PostsolveStatus::BoundViolation;
      result.errorMessage = "Non-finite primal value on variable " + originalModel.variables[i].name;
      result.maxBoundResidual = std::numeric_limits<double>::infinity();
      return false;
    }
  }

  // 2. Validate Variable Bounds & Integrality
  for (std::size_t i = 0; i < originalModel.variables.size(); ++i) {
    const auto& var = originalModel.variables[i];
    double val = x[i];

    // Check Lower Bound
    if (val < var.lowerBound - tolerance_) {
      double diff = var.lowerBound - val;
      result.maxBoundResidual = std::max(result.maxBoundResidual, diff);
      result.status = PostsolveStatus::BoundViolation;
      result.errorMessage = "Lower bound violation on variable " + var.name;
    }

    // Check Upper Bound
    if (val > var.upperBound + tolerance_) {
      double diff = val - var.upperBound;
      result.maxBoundResidual = std::max(result.maxBoundResidual, diff);
      result.status = PostsolveStatus::BoundViolation;
      result.errorMessage = "Upper bound violation on variable " + var.name;
    }

    // Check Integrality
    if (var.type == model::VariableType::Integer || var.type == model::VariableType::Binary) {
      double rounded = std::round(val);
      if (std::abs(val - rounded) > tolerance_) {
        result.status = PostsolveStatus::IntegralityViolation;
        result.errorMessage = "Integrality violation on variable " + var.name;
      }
    }
  }

  if (result.status == PostsolveStatus::BoundViolation ||
      result.status == PostsolveStatus::IntegralityViolation) {
    return false;
  }

  // Validate Constraints
  for (const auto& constraint : originalModel.constraints) {
    double activity = 0.0;

    for (const auto& term : constraint.linearTerms) {
  if (term.variableIndex < 0 ||
      static_cast<std::size_t>(term.variableIndex) >= x.size()) {
    result.status = PostsolveStatus::InvalidMapping;
    result.errorMessage =
        "Invalid mapping: constraint contains an out-of-range variable index.";
    return false;
  }

  activity += term.value * x[term.variableIndex];
}

    // Check non-finite activity
    if (!std::isfinite(activity)) {
      result.status = PostsolveStatus::ConstraintViolation;
      result.errorMessage = "Non-finite activity on constraint " + constraint.name;
      result.maxConstraintResidual = std::numeric_limits<double>::infinity();
      return false;
    }

    // Check Lower Bound violation
    if (activity < constraint.lowerBound - tolerance_) {
      double diff = constraint.lowerBound - activity;
      result.maxConstraintResidual = std::max(result.maxConstraintResidual, diff);
      result.status = PostsolveStatus::ConstraintViolation;
      result.errorMessage = "Constraint lower bound violation on " + constraint.name;
    }

    // Check Upper Bound violation
    if (activity > constraint.upperBound + tolerance_) {
      double diff = activity - constraint.upperBound;
      result.maxConstraintResidual = std::max(result.maxConstraintResidual, diff);
      result.status = PostsolveStatus::ConstraintViolation;
      result.errorMessage = "Constraint upper bound violation on " + constraint.name;
    }
  }

  if (result.status == PostsolveStatus::ConstraintViolation) {
    return false;
  }

  return true;
}

}  // namespace postsolve