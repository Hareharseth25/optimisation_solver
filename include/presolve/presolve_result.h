#pragma once

#include "model/model.h"
#include "presolve/transformation.h"

#include <cstddef>
#include <vector>

namespace presolve {

struct PresolveResult {
  model::Model model;

  bool infeasible = false;
  bool converged = true;

  std::size_t originalVariables = 0;
  std::size_t originalConstraints = 0;

  std::size_t presolvedVariables = 0;
  std::size_t presolvedConstraints = 0;

  std::vector<Transformation> transformations;

  PostsolveMetadata postsolve;
};

} // namespace presolve