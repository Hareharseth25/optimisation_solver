#pragma once

// pdlp_adapter.h -- the ONLY place model::Model and pdlp::CompiledLp meet.
//
// PDLP is deliberately decoupled from the shared IR: it takes its own
// CompiledLp so it can be built, tested and benchmarked standalone. That
// decoupling is correct, but it means nothing can reach the engine without a
// translation layer. This is that layer.
//
// THE ADAPTER IS TWO HALVES AND BOTH ARE REQUIRED.
//
//   toCompiledLp()   model::Model            -> pdlp::CompiledLp
//   toModelSolution() pdlp::PdlpResult       -> solution in MODEL coordinates
//
// The return half is the one people forget, and it carries TWO independent
// corrections that must not be conflated:
//
//   1. the sense flip -- a maximisation was negated going in, so negate the
//      objective coming back;
//   2. the dual convention -- PDLP's saddle point uses
//      `stationarity = c + A^T y`, making its y the negative of the usual
//      Lagrange multiplier, while callers expect a shadow price d(obj)/d(rhs).
//
// The corrections compose rather than cancel: shadow price = -y for a
// minimisation and +y for a maximisation. Verified against HiGHS in both
// senses. Get it wrong and strong duality no longer reproduces the objective
// from the duals, and a binding `<=` row of a maximisation reports a negative
// shadow price -- claiming that relaxing a constraint hurts.

#include <string>
#include <vector>

#include "model/model.h"
#include "pdlp/compiled_lp.h"
#include "pdlp/pdlp_result.h"

namespace adapter {

// What the adapter did, so the caller and the log can report it.
struct PdlpTranslation {
  bool ok = false;
  std::string error;

  // True when the model was a maximisation and we negated the objective.
  // toModelSolution() needs this to undo it, so keep the object around.
  bool objectiveNegated = false;

  // Integer variables were relaxed to continuous. NEVER done silently:
  // the caller must have asked for it via relaxIntegrality. PDLP is an LP
  // engine; handing it a MILP and reporting "Optimal" would be a wrong answer.
  bool integralityRelaxed = false;
  int relaxedVariableCount = 0;

  // Diagnostics worth surfacing.
  int duplicateTermsSummed = 0;   // same (row, column) written twice
  int zeroTermsDropped = 0;
  int emptyRows = 0;              // constraints with no terms
  int emptyColumns = 0;           // variables in no constraint

  // Index maps. Identity today because the adapter does not filter anything --
  // that is presolve's job, per the layer contract. They exist so that a future
  // filtering adapter does not change this signature.
  std::vector<int> modelToCompiledVar;
  std::vector<int> modelToCompiledRow;
};

struct AdapterOptions {
  // Relax Integer and Binary variables to continuous. Must be explicit.
  bool relaxIntegrality = false;

  // Reject rather than translate a model carrying quadratic objective terms.
  // PDLP solves LPs only; silently dropping the quadratic part would change
  // the problem being solved.
  bool rejectQuadratic = true;
};

// Forward translation. On failure `out` is left unspecified and the returned
// translation carries the reason.
PdlpTranslation toCompiledLp(const model::Model& in, pdlp::CompiledLp& out,
                             const AdapterOptions& opts = {});

// A solution expressed in the ORIGINAL model's coordinates and sign convention.
struct ModelSolution {
  bool ok = false;
  std::string statusMessage;
  pdlp::PdlpStatus status = pdlp::PdlpStatus::InvalidProblem;

  std::vector<double> variableValues;   // length model.variables.size()
  // Shadow prices, d(objective)/d(right-hand side), in the model's own sense --
  // the same convention HiGHS reports. Length model.constraints.size().
  std::vector<double> constraintDuals;
  double objectiveValue = 0.0;          // in the model's own sense

  // Integrality violation of the returned point, computed only when the model
  // had integer variables and they were relaxed. Non-zero means the caller was
  // handed an LP relaxation, not a MILP solution.
  double maxIntegralityViolation = 0.0;
};

ModelSolution toModelSolution(const model::Model& in,
                              const PdlpTranslation& translation,
                              const pdlp::PdlpResult& result);

}  // namespace adapter
