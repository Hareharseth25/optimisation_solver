// End-to-end: model::Model -> adapter -> PDLP -> back to model coordinates.
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

#include "adapter/pdlp_adapter.h"
#include "pdlp/pdlp_solver.h"

namespace {
int checks = 0, failures = 0;
void ck(bool c, const char* w, int line) {
  ++checks;
  if (!c) { ++failures; std::fprintf(stderr, "FAIL line %d: %s\n", line, w); }
}
#define CHECK(c) ck((c), #c, __LINE__)
#define NEAR(a,b,t) ck(std::fabs((a)-(b)) < (t), #a " ~= " #b, __LINE__)

const double INF = std::numeric_limits<double>::infinity();

model::Variable var(const char* n, double lo, double hi,
                    model::VariableType t = model::VariableType::Continuous) {
  model::Variable v; v.name = n; v.lowerBound = lo; v.upperBound = hi; v.type = t;
  return v;
}
model::Constraint row(const char* n, double lo, double hi,
                      std::vector<model::LinearTerm> ts) {
  model::Constraint c; c.name = n; c.lowerBound = lo; c.upperBound = hi;
  c.linearTerms = std::move(ts); return c;
}

// min 3x + 5y  s.t.  2x + y >= 4,  x + 3y <= 6,  0<=x,y<=10
// optimum: x=2, y=0, obj 6
void testMinimize() {
  model::Model m; m.name = "min_lp";
  m.variables = { var("x",0,10), var("y",0,10) };
  m.objective.sense = model::ObjectiveSense::Minimize;
  m.objective.linearTerms = { {0,3.0}, {1,5.0} };
  m.constraints = { row("C1",4.0,INF,{{0,2.0},{1,1.0}}),
                    row("C2",-INF,6.0,{{0,1.0},{1,3.0}}) };
  CHECK(m.validate());

  pdlp::CompiledLp lp;
  const auto tr = adapter::toCompiledLp(m, lp);
  CHECK(tr.ok);
  if (!tr.ok) { std::fprintf(stderr, "  %s\n", tr.error.c_str()); return; }
  CHECK(!tr.objectiveNegated);
  CHECK(lp.numRows() == 2 && lp.numColumns() == 2);

  pdlp::PdlpOptions o; o.threadCount = 1;
  const auto r = pdlp::PdlpSolver{}.solve(lp, o);
  const auto s = adapter::toModelSolution(m, tr, r);
  std::printf("  min: status=%s obj=%.8f x=%.6f y=%.6f\n",
              pdlp::toString(r.status), s.objectiveValue,
              s.variableValues[0], s.variableValues[1]);
  CHECK(r.status == pdlp::PdlpStatus::Optimal);
  NEAR(s.objectiveValue, 6.0, 1e-4);
  NEAR(s.variableValues[0], 2.0, 1e-4);
}

// SAME feasible region, MAXIMISE 3x + 5y.
// Along x+3y=6 the objective is 18-4y, so the optimum is (6,0) -> obj 18.
// This is the test that catches a missing sense flip.
void testMaximize() {
  model::Model m; m.name = "max_lp";
  m.variables = { var("x",0,10), var("y",0,10) };
  m.objective.sense = model::ObjectiveSense::Maximize;
  m.objective.linearTerms = { {0,3.0}, {1,5.0} };
  m.constraints = { row("C1",4.0,INF,{{0,2.0},{1,1.0}}),
                    row("C2",-INF,6.0,{{0,1.0},{1,3.0}}) };

  pdlp::CompiledLp lp;
  const auto tr = adapter::toCompiledLp(m, lp);
  CHECK(tr.ok);
  CHECK(tr.objectiveNegated);
  NEAR(lp.objective[0], -3.0, 1e-15);   // negated going in
  NEAR(lp.objective[1], -5.0, 1e-15);

  pdlp::PdlpOptions o; o.threadCount = 1;
  const auto r = pdlp::PdlpSolver{}.solve(lp, o);
  const auto s = adapter::toModelSolution(m, tr, r);
  std::printf("  max: status=%s obj=%.8f x=%.6f y=%.6f\n",
              pdlp::toString(r.status), s.objectiveValue,
              s.variableValues[0], s.variableValues[1]);
  CHECK(r.status == pdlp::PdlpStatus::Optimal);
  // objective must come back POSITIVE in the model's own sense
  NEAR(s.objectiveValue, 18.0, 1e-3);
  NEAR(s.variableValues[0], 6.0, 1e-3);
}

void testObjectiveOffsetIsNegatedToo() {
  model::Model m;
  m.variables = { var("x",0,1) };
  m.objective.sense = model::ObjectiveSense::Maximize;
  m.objective.offset = 7.0;
  m.objective.linearTerms = { {0,1.0} };
  m.constraints = { row("C",-INF,1.0,{{0,1.0}}) };

  pdlp::CompiledLp lp;
  const auto tr = adapter::toCompiledLp(m, lp);
  CHECK(tr.ok);
  NEAR(lp.objectiveOffset, -7.0, 1e-15);
}

void testIntegersRejectedByDefault() {
  model::Model m;
  m.variables = { var("b",0,1,model::VariableType::Binary) };
  m.objective.linearTerms = { {0,1.0} };
  m.constraints = { row("C",-INF,1.0,{{0,1.0}}) };

  pdlp::CompiledLp lp;
  const auto tr = adapter::toCompiledLp(m, lp);
  CHECK(!tr.ok);                       // must NOT silently solve a MILP
  CHECK(tr.error.find("integer") != std::string::npos);

  adapter::AdapterOptions o; o.relaxIntegrality = true;
  const auto tr2 = adapter::toCompiledLp(m, lp, o);
  CHECK(tr2.ok);
  CHECK(tr2.integralityRelaxed && tr2.relaxedVariableCount == 1);
}

void testQuadraticRejected() {
  model::Model m;
  m.variables = { var("x",0,1) };
  m.objective.quadraticTerms = { {0,0,1.0} };
  m.constraints = { row("C",-INF,1.0,{{0,1.0}}) };
  pdlp::CompiledLp lp;
  CHECK(!adapter::toCompiledLp(m, lp).ok);
}

void testDuplicatesAndZeros() {
  model::Model m;
  m.variables = { var("x",0,10) };
  m.objective.linearTerms = { {0,1.0} };
  m.constraints = { row("C",-INF,5.0,{{0,2.0},{0,3.0},{0,0.0}}) };
  pdlp::CompiledLp lp;
  const auto tr = adapter::toCompiledLp(m, lp);
  CHECK(tr.ok);
  CHECK(tr.duplicateTermsSummed == 1);
  CHECK(tr.zeroTermsDropped == 1);
  CHECK(lp.matrix.nonzeros() == 1);        // 2+3 merged into one entry
}

void testFreeVariableStaysInfinite() {
  model::Model m;
  m.variables = { var("f",-INF,INF) };
  m.objective.linearTerms = { {0,1.0} };
  m.constraints = { row("C",0.0,1.0,{{0,1.0}}) };
  pdlp::CompiledLp lp;
  CHECK(adapter::toCompiledLp(m, lp).ok);
  // A finite sentinel here would silently bound a free variable.
  CHECK(std::isinf(lp.variableLower[0]));
  CHECK(std::isinf(lp.variableUpper[0]));
}

void testBadIndexRejected() {
  model::Model m;
  m.variables = { var("x",0,1) };
  m.objective.linearTerms = { {0,1.0} };
  m.constraints = { row("C",-INF,1.0,{{5,1.0}}) };
  pdlp::CompiledLp lp;
  const auto tr = adapter::toCompiledLp(m, lp);
  CHECK(!tr.ok);
}
}  // namespace


// ---------------------------------------------------------------------------
// Dual sign convention.
//
// The original adapter passed duals through on a minimisation and negated them
// on a maximisation. Both were wrong: PDLP's `stationarity = c + A^T y` makes
// its y the negative of the usual multiplier, so the correct mapping is the
// other way round. Nothing here caught it because nothing here looked at duals.
// ---------------------------------------------------------------------------
static void testDualSignMatchesShadowPrices() {
  auto build = [&](bool maximise) {
    model::Model m;
    auto V = [&](const char* nm) {
      model::Variable v; v.name = nm; v.type = model::VariableType::Continuous;
      v.lowerBound = 0.0; v.upperBound = INF; m.variables.push_back(v);
    };
    V("x"); V("y");
    auto C = [&](const char* nm, double up, std::vector<model::LinearTerm> t) {
      model::Constraint c; c.name = nm; c.lowerBound = -INF; c.upperBound = up;
      c.linearTerms = std::move(t); m.constraints.push_back(c);
    };
    C("c0", 14.0, {{0, 2.0}, {1, 1.0}});
    C("c1", 28.0, {{0, 4.0}, {1, 5.0}});
    C("c2", 30.0, {{0, 2.0}, {1, 5.0}});
    m.objective.sense = maximise ? model::ObjectiveSense::Maximize
                                 : model::ObjectiveSense::Minimize;
    const double s = maximise ? 1.0 : -1.0;
    m.objective.linearTerms = {{0, s * 3.0}, {1, s * 4.0}};
    return m;
  };
  auto solveIt = [&](const model::Model& m) {
    pdlp::CompiledLp lp;
    const auto tr = adapter::toCompiledLp(m, lp);
    CHECK(tr.ok);
    pdlp::PdlpOptions o;
    o.primalTolerance = 1e-9; o.dualTolerance = 1e-9; o.gapTolerance = 1e-9;
    return adapter::toModelSolution(m, tr, pdlp::PdlpSolver{}.solve(lp, o));
  };

  // HiGHS marginals for min -3x-4y over this system are [0, -0.8, 0].
  const auto mn = solveIt(build(false));
  // HiGHS marginal for this binding row on the minimisation is -0.8.
  NEAR(mn.constraintDuals[1], -0.8, 1e-5);

  // For the equivalent maximisation they are [0, +0.8, 0].
  const auto mx = solveIt(build(true));
  // ...and +0.8 on the equivalent maximisation.
  NEAR(mx.constraintDuals[1], 0.8, 1e-5);

  // Strong duality must reproduce the objective from the duals.
  const model::Model m = build(true);
  double bty = 0.0;
  for (std::size_t i = 0; i < m.constraints.size(); ++i) {
    bty += mx.constraintDuals[i] * m.constraints[i].upperBound;
  }
  // Strong duality: the duals must reproduce the objective.
  NEAR(mx.objectiveValue, bty, 1e-4);
}

int main() {
  testDualSignMatchesShadowPrices();
  testMinimize();
  testMaximize();
  testObjectiveOffsetIsNegatedToo();
  testIntegersRejectedByDefault();
  testQuadraticRejected();
  testDuplicatesAndZeros();
  testFreeVariableStaysInfinite();
  testBadIndexRejected();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
