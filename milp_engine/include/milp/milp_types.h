#pragma once

namespace milp {

// Column/row index type used throughout the MILP engine's LP relaxation
// layer. A plain alias rather than a strong type: the engine interoperates
// with model::Model, whose LinearTerm::variableIndex is already a plain int.
using Index = int;

constexpr double kDefaultFeasibilityTolerance = 1e-9;
constexpr double kDefaultOptimalityTolerance = 1e-9;

} // namespace milp
