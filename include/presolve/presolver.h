#pragma once

#include "model/model.h"
#include "presolve/presolve_result.h"

#include <cstddef>
#include <vector>

namespace presolve {

class Presolver {
public:
  PresolveResult run(const model::Model &input);

private:
  void removeVariable(model::Model &model, std::size_t variableIndex,
                      std::vector<std::size_t> &currentToOrigVar);

  bool removeZeroCoefficients(model::Model &model);

  bool fixVariable(model::Model &model, std::size_t variableIndex,
                   double value, PresolveResult &result,
                   std::vector<std::size_t> &currentToOrigVar);

  bool processSingletonRow(model::Model &model, std::size_t constraintIndex,
                           PresolveResult &result,
                           const std::vector<std::size_t> &currentToOrigVar,
                           std::vector<std::size_t> &currentToOrigConstraint);

  bool detectSimpleInfeasibility(const model::Model &model) const;

  bool tightenBounds(model::Model &model, PresolveResult &result,
                     const std::vector<std::size_t> &currentToOrigVar,
                     const std::vector<std::size_t> &currentToOrigConstraint);

  bool removeRedundantConstraints(model::Model &model, PresolveResult &result,
                                  std::vector<std::size_t> &currentToOrigConstraint);

  bool removeDuplicateAndParallelRows(model::Model &model,
                                      PresolveResult &result,
                                      std::vector<std::size_t> &currentToOrigConstraint);

  // ==========================================================
  // Helpers
  // ==========================================================

  double coefficientForVariable(const model::Constraint &constraint,
                                std::size_t variableIndex) const;

  double minConstraintActivity(const model::Constraint &constraint,
                               const model::Model &model) const;

  double maxConstraintActivity(const model::Constraint &constraint,
                               const model::Model &model) const;

  bool constraintsEquivalent(const model::Constraint &a,
                             const model::Constraint &b) const;

  bool constraintsParallel(const model::Constraint &a,
                           const model::Constraint &b) const;
};

} // namespace presolve
