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
};

}  // namespace postsolve

#endif  // POSTSOLVE_POSTSOLVER_H_