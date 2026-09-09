#include "adapter/pdlp_adapter.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <unordered_map>

#include "pdlp/sparse_matrix.h"

namespace adapter {
namespace {

constexpr double kZeroTolerance = 1e-12;

bool isIntegral(model::VariableType t) {
  return t == model::VariableType::Integer || t == model::VariableType::Binary;
}

}  // namespace

PdlpTranslation toCompiledLp(const model::Model& in, pdlp::CompiledLp& out,
                             const AdapterOptions& opts) {
  PdlpTranslation tr;

  const int n = static_cast<int>(in.variables.size());
  const int m = static_cast<int>(in.constraints.size());

  // ---- reject what PDLP cannot solve, loudly -------------------------------
  if (opts.rejectQuadratic && !in.objective.quadraticTerms.empty()) {
    tr.error =
        "model has quadratic objective terms; PDLP solves linear programs only";
    return tr;
  }

  int integralCount = 0;
  for (const model::Variable& v : in.variables) {
    if (isIntegral(v.type)) ++integralCount;
  }
  if (integralCount > 0 && !opts.relaxIntegrality) {
    std::ostringstream e;
    e << "model has " << integralCount
      << " integer variables; route it to branch-and-cut, or set "
         "relaxIntegrality to solve the LP relaxation deliberately";
    tr.error = e.str();
    return tr;
  }
  tr.integralityRelaxed = integralCount > 0;
  tr.relaxedVariableCount = integralCount;

  // ---- objective sense -----------------------------------------------------
  //
  // CompiledLp is MINIMISATION ONLY. For a maximisation model we minimise the
  // negated objective:
  //
  //     max (c'x + k)   ==   -min (-c'x - k)
  //
  // so BOTH the coefficients and the offset are negated here, and the objective
  // value is negated again in toModelSolution(). The duals need a separate
  // correction for PDLP's sign convention -- see the note there; the two do not
  // cancel, and treating them as one substitution is how the sign gets lost.
  const bool maximise = (in.objective.sense == model::ObjectiveSense::Maximize);
  tr.objectiveNegated = maximise;
  const double sign = maximise ? -1.0 : 1.0;

  out.objective.assign(static_cast<std::size_t>(n), 0.0);
  for (const model::LinearTerm& t : in.objective.linearTerms) {
    if (t.variableIndex < 0 || t.variableIndex >= n) {
      std::ostringstream e;
      e << "objective references variable index " << t.variableIndex
        << ", outside [0," << n << ")";
      tr.error = e.str();
      return tr;
    }
    // Duplicate objective terms for the same variable are summed, matching the
    // convention used everywhere else in the IR.
    out.objective[static_cast<std::size_t>(t.variableIndex)] += sign * t.value;
  }
  out.objectiveOffset = sign * in.objective.offset;

  // ---- variable bounds -----------------------------------------------------
  //
  // Both sides use true IEEE infinity for absent bounds and PDLP tests them
  // with std::isfinite(), so bounds copy across unchanged. Do NOT introduce a
  // finite sentinel here: std::isfinite(1e30) is true, and a free variable
  // would silently become one bounded at 1e30.
  out.variableLower.resize(static_cast<std::size_t>(n));
  out.variableUpper.resize(static_cast<std::size_t>(n));
  for (int j = 0; j < n; ++j) {
    const model::Variable& v = in.variables[static_cast<std::size_t>(j)];
    out.variableLower[static_cast<std::size_t>(j)] = v.lowerBound;
    out.variableUpper[static_cast<std::size_t>(j)] = v.upperBound;
    if (v.lowerBound > v.upperBound) {
      std::ostringstream e;
      e << "variable " << j << " (" << v.name << ") has lower bound "
        << v.lowerBound << " above upper bound " << v.upperBound;
      tr.error = e.str();
      return tr;
    }
  }

  // ---- row bounds ----------------------------------------------------------
  //
  // Both IRs already store two-sided bounds, so ranged, equality and one-sided
  // rows all copy across with no case analysis. This is the payoff of the IR
  // having chosen [lower, upper] over (sense, rhs, range).
  out.rowLower.resize(static_cast<std::size_t>(m));
  out.rowUpper.resize(static_cast<std::size_t>(m));
  for (int i = 0; i < m; ++i) {
    const model::Constraint& c = in.constraints[static_cast<std::size_t>(i)];
    out.rowLower[static_cast<std::size_t>(i)] = c.lowerBound;
    out.rowUpper[static_cast<std::size_t>(i)] = c.upperBound;
    if (c.lowerBound > c.upperBound) {
      std::ostringstream e;
      e << "constraint " << i << " (" << c.name << ") has lower bound "
        << c.lowerBound << " above upper bound " << c.upperBound;
      tr.error = e.str();
      return tr;
    }
    if (c.linearTerms.empty()) ++tr.emptyRows;
  }

  // ---- matrix --------------------------------------------------------------
  //
  // SparseMatrix::fromTriplets sums duplicates, drops exact zeros and accepts
  // unsorted input, so we hand it the terms as they appear. We count the
  // duplicates ourselves purely so the log can report them.
  std::vector<pdlp::MatrixTriplet> triplets;
  std::size_t rawTerms = 0;
  for (const model::Constraint& c : in.constraints) rawTerms += c.linearTerms.size();
  triplets.reserve(rawTerms);

  std::unordered_map<long long, int> seen;
  seen.reserve(rawTerms * 2);
  std::vector<char> columnUsed(static_cast<std::size_t>(n), 0);

  for (int i = 0; i < m; ++i) {
    const model::Constraint& c = in.constraints[static_cast<std::size_t>(i)];
    for (const model::LinearTerm& t : c.linearTerms) {
      if (t.variableIndex < 0 || t.variableIndex >= n) {
        std::ostringstream e;
        e << "constraint " << i << " (" << c.name << ") references variable index "
          << t.variableIndex << ", outside [0," << n << ")";
        tr.error = e.str();
        return tr;
      }
      if (!std::isfinite(t.value)) {
        std::ostringstream e;
        e << "constraint " << i << " has a non-finite coefficient";
        tr.error = e.str();
        return tr;
      }
      if (std::abs(t.value) < kZeroTolerance) {
        ++tr.zeroTermsDropped;
        continue;
      }
      const long long key =
          static_cast<long long>(i) * n + t.variableIndex;
      if (++seen[key] > 1) ++tr.duplicateTermsSummed;
      columnUsed[static_cast<std::size_t>(t.variableIndex)] = 1;
      triplets.push_back(pdlp::MatrixTriplet{i, t.variableIndex, t.value});
    }
  }
  for (int j = 0; j < n; ++j) {
    if (!columnUsed[static_cast<std::size_t>(j)]) ++tr.emptyColumns;
  }

  out.matrix = pdlp::SparseMatrix::fromTriplets(m, n, std::move(triplets));

  // ---- index maps ----------------------------------------------------------
  // Identity: the adapter filters nothing. Reduction is presolve's job, per the
  // layer contract. These exist so a filtering adapter later would not change
  // this signature.
  tr.modelToCompiledVar.resize(static_cast<std::size_t>(n));
  for (int j = 0; j < n; ++j) tr.modelToCompiledVar[static_cast<std::size_t>(j)] = j;
  tr.modelToCompiledRow.resize(static_cast<std::size_t>(m));
  for (int i = 0; i < m; ++i) tr.modelToCompiledRow[static_cast<std::size_t>(i)] = i;

  // Let CompiledLp's own validator have the final word rather than trusting
  // ourselves. It throws; convert that into our error channel.
  try {
    out.validate();
  } catch (const std::exception& ex) {
    tr.error = std::string("CompiledLp rejected the translation: ") + ex.what();
    return tr;
  }

  tr.ok = true;
  return tr;
}

ModelSolution toModelSolution(const model::Model& in,
                              const PdlpTranslation& translation,
                              const pdlp::PdlpResult& result) {
  ModelSolution sol;
  sol.status = result.status;
  sol.statusMessage = result.statusMessage;

  const int n = static_cast<int>(in.variables.size());
  const int m = static_cast<int>(in.constraints.size());

  if (result.primal.size() != static_cast<std::size_t>(n)) {
    sol.statusMessage = "result primal vector has the wrong length";
    return sol;
  }

  sol.variableValues = result.primal;

  sol.constraintDuals.assign(static_cast<std::size_t>(m), 0.0);
  if (result.rowDual.size() == static_cast<std::size_t>(m)) {
    sol.constraintDuals = result.rowDual;
  }

  // UNDO THE SENSE FLIP, and convert PDLP's dual convention to the caller's.
  //
  // These are two separate corrections and they do NOT cancel.
  //
  // 1. Objective. It was negated going in, so negate it coming back.
  //
  // 2. Duals. PDLP's saddle point uses `stationarity = c + A^T y`, which makes
  //    its y the NEGATIVE of the usual Lagrange multiplier (most formulations
  //    use L = c'x - y'(Ax - b)). Callers expect a shadow price, d(objective)
  //    per unit of right-hand side, which is what HiGHS and every textbook
  //    report. So:
  //
  //        minimisation:  shadow price = -y_pdlp
  //        maximisation:  shadow price = +y_pdlp
  //
  //    because the extra negation from the sense flip cancels the convention
  //    negation. Verified against HiGHS on both senses: for min -3x-4y with
  //    2x+y<=14, 4x+5y<=28, 2x+5y<=30 the marginals are [0,-0.8,0], and for the
  //    equivalent max they are [0,+0.8,0].
  //
  //    Getting this backwards is not a cosmetic error: a negative shadow price
  //    on a binding `<=` row of a maximisation claims that relaxing the
  //    constraint makes the objective worse, and strong duality fails to
  //    reproduce the objective from the duals.
  const double dualSign = translation.objectiveNegated ? 1.0 : -1.0;
  for (double& d : sol.constraintDuals) d *= dualSign;

  if (translation.objectiveNegated) {
    sol.objectiveValue = -result.primalObjective;
  } else {
    sol.objectiveValue = result.primalObjective;
  }

  // If integrality was relaxed, measure how far the point is from integral so
  // the caller cannot mistake an LP relaxation for a MILP solution.
  if (translation.integralityRelaxed) {
    for (int j = 0; j < n; ++j) {
      const model::Variable& v = in.variables[static_cast<std::size_t>(j)];
      if (!isIntegral(v.type)) continue;
      const double x = sol.variableValues[static_cast<std::size_t>(j)];
      sol.maxIntegralityViolation =
          std::max(sol.maxIntegralityViolation, std::abs(x - std::round(x)));
    }
  }

  sol.ok = (result.status == pdlp::PdlpStatus::Optimal);
  return sol;
}

}  // namespace adapter
