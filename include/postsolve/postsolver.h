#ifndef POSTSOLVE_POSTSOLVER_H_
#define POSTSOLVE_POSTSOLVER_H_

#include <vector>
#include <string>
#include <cmath>
#include <algorithm>

#include "model/model.h"
#include "presolve/presolve_result.h"

namespace postsolve {

constexpr double DEFAULT_POSTSOLVE_TOLERANCE = 1e-6;

enum class PostsolveStatus {
  Success,
  InfeasiblePresolve,
  BoundViolation,
  ConstraintViolation,
  IntegralityViolation,
  InvalidMapping,
  InternalError
};

struct PostsolveResult {
  PostsolveStatus status = PostsolveStatus::InternalError;
  std::string errorMessage;
  
  std::vector<double> primalSolution; // In original variable index order
  double originalObjectiveValue = 0.0;
  
  double maxBoundResidual = 0.0;
  double maxConstraintResidual = 0.0;

  // ---- Dual reconstruction, in ORIGINAL index order ----
  //
  // Only populated when the caller supplied reduced-space duals AND every
  // transformation between the two spaces could be reversed. When it could
  // not, dualsAvailable stays false and dualsUnavailableReason says why --
  // deliberately, rather than emitting zeros or reduced-space numbers, either
  // of which a caller would read as a valid answer.
  bool dualsAvailable = false;
  std::string dualsUnavailableReason;

  // Shadow prices, one per ORIGINAL constraint: d(objective)/d(right-hand
  // side) in the model's own objective sense.
  std::vector<double> constraintDuals;

  // Reduced costs, one per ORIGINAL variable:
  //     d_j = grad_j f(x*) - sum_i a_ij * y_i
  // For a variable strictly inside its bounds this is zero; at a bound it is
  // the bound's multiplier, signed by the objective sense.
  std::vector<double> reducedCosts;

  // Worst violation of the ORIGINAL model's optimality conditions by the
  // reconstructed multipliers -- stationarity, dual sign feasibility and
  // complementary slackness. Checked before duals are published.
  double maxDualResidual = 0.0;

  bool isSuccess() const { return status == PostsolveStatus::Success; }
};

class Postsolver {
 public:
  explicit Postsolver(double tolerance = DEFAULT_POSTSOLVE_TOLERANCE)
      : tolerance_(tolerance) {}

  PostsolveResult process(
      const model::Model& originalModel,
      const presolve::PresolveResult& presolveResult,
      const std::vector<double>& presolvedPrimalSolution);

  // As above, and additionally reconstructs original-space duals and reduced
  // costs from the reduced-space row duals the engine produced.
  //
  // `presolvedConstraintDuals` is indexed by the PRESOLVED constraints and
  // must use the shadow-price convention: y_i = d(objective)/d(rhs_i) in the
  // model's own sense. Pass it empty to skip dual reconstruction entirely.
  PostsolveResult process(
      const model::Model& originalModel,
      const presolve::PresolveResult& presolveResult,
      const std::vector<double>& presolvedPrimalSolution,
      const std::vector<double>& presolvedConstraintDuals);

  // Gradient of the model objective at x, following the Model convention
  //   f(x) = offset + sum_j c_j x_j + sum q_ij x_i x_j
  // where QuadraticTerm.value is the direct coefficient of x_i*x_j with no
  // implicit 1/2. So a diagonal term q_kk contributes 2*q_kk*x_k, and an
  // off-diagonal q_ij contributes q_ij*x_j to grad_i and q_ij*x_i to grad_j.
  [[nodiscard]] std::vector<double> objectiveGradient(
      const model::Model& model,
      const std::vector<double>& x) const;

  double evaluateObjective(
      const model::Model& model,
      const std::vector<double>& x) const;

 private:
  double tolerance_;

  // Validates that the mapping/metadata produced by presolve is internally
  // consistent and safe to dereference before it is used to reconstruct a
  // solution. On failure, populates result with PostsolveStatus::InvalidMapping
  // and a descriptive errorMessage, and returns false.
  bool validateMapping(
      const model::Model& originalModel,
      const presolve::PresolveResult& presolveResult,
      const std::vector<double>& presolvedPrimalSolution,
      PostsolveResult& result) const;

  bool validateSolution(
      const model::Model& originalModel,
      const std::vector<double>& x,
      PostsolveResult& result) const;

  // Walks the transformation log in reverse, moving multipliers that presolve
  // parked on derived variable bounds back onto the rows they came from.
  void reconstructDuals(
      const model::Model& originalModel,
      const presolve::PresolveResult& presolveResult,
      const std::vector<double>& presolvedConstraintDuals,
      PostsolveResult& result) const;

  // Measures how badly the reconstructed multipliers violate the ORIGINAL
  // model's optimality conditions. Returns the worst violation found.
  [[nodiscard]] double dualResidual(
      const model::Model& originalModel,
      const PostsolveResult& result) const;
};

}  // namespace postsolve

#endif  // POSTSOLVE_POSTSOLVER_H_