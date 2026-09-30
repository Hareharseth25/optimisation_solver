# Barrier (primal-dual interior-point) engine

`barrier_engine` solves linear programs and convex quadratic programs

```
minimize    c'x + 0.5 x'Qx
subject to  A x = b,   l <= x <= u        (Q symmetric positive semidefinite)
```

with an infeasible-start primal-dual interior-point method, built from scratch:
no third-party solver and no external linear-algebra library.

```
optimsolver solve model.mps --solver barrier        # aliases: ipm, interior_point
```

## Method

| Component | Choice | Source |
|---|---|---|
| Path following | Mehrotra predictor-corrector, adaptive centering `mu = (g_a/g)^2 * (g_a/n)` | Mehrotra 1992 |
| Extra correctors | Gondzio multiple centrality correctors, reusing one factorisation | Gondzio 1996 |
| Newton system | Regularised **augmented system**, not normal equations | Altman & Gondzio 1999 |
| Factorisation | Sparse LDL' of a symmetric quasidefinite matrix, no pivoting | Vanderbei 1995 |
| Ordering | Approximate minimum degree on a quotient graph | Amestoy, Davis & Duff 1996 |
| Accuracy | Iterative refinement on every solve | standard practice |
| Vertex recovery | Crossover to the dual simplex (LP only) | Megiddo 1991; Bixby & Saltzman 1994 |
| Active-set recovery | Polishing to the identified active set, with active-set correction | as in OSQP; El-Bakry, Tapia & Zhang 1994 |

### Why the augmented system

Each iteration solves

```
[ -(Q + D + Rp)   A' ] [dx]   [ -rd_hat ]        D = Xl^-1 Zl + Xu^-1 Zu
[      A          Rd ] [dy] = [   rp    ]
```

The common alternative, normal equations `A D^-1 A'`, has two failure modes this
avoids. A single dense column of `A` makes `A D^-1 A'` completely dense, and a
free variable has `D = 0`, so `D^-1` does not exist. With primal and dual
regularisation `Rp, Rd > 0` the augmented matrix is **quasidefinite**, and
Vanderbei's theorem says every symmetric permutation of a quasidefinite matrix
has a stable LDL' factorisation. That is what lets the fill-reducing ordering be
used as-is, with no pivoting and no inertia-controlling factorisation. A convex
QP drops `Q` into the leading block and changes nothing else.

Only the two diagonal blocks change between iterations. The pattern, the AMD
ordering and the symbolic analysis are computed once; each iteration pays only
for the numeric factorisation.

### Numerical safeguards

- **Inertia check.** A quasidefinite matrix of this shape has exactly `n`
  negative pivots. Every factorisation is checked for that, and a mismatch
  escalates the regularisation and refactorises. Escalation relaxes back toward
  the requested level on later iterations rather than ratcheting.
- **Iterative refinement.** An unpivoted LDL' of a quasidefinite matrix completes
  stably, but its accuracy degrades as the definite blocks shrink, and the method
  drives `Rd` to ~1e-8 deliberately. Measured on a 70+40 augmented system with
  `Rd = 1e-8`: the raw solve left a residual of 1.1e-8 where dense Gaussian
  elimination with partial pivoting reached 2.4e-15; two rounds of refinement
  brought it to 6.7e-16.
- **Fixed variables** have no interior, so the adapter substitutes them out
  before the solver sees the problem.
- **Convexity** is re-checked when the barrier is forced, because a forced
  engine bypasses the dispatcher's own check. On a nonconvex Hessian the leading
  block is indefinite, and heavy regularisation would return the optimum of a
  different problem.

## Statuses, and what they do not claim

| Barrier status | Pipeline status | Meaning |
|---|---|---|
| `Optimal` | `Optimal` | relative primal, dual and gap residuals all within tolerance |
| `IterationLimit`, `TimeLimit` | `LimitReached` | budget exhausted |
| `SuspectedInfeasibleOrUnbounded` | `LimitReached` | see below |
| `NumericalFailure` | `NumericalFailure` | factorisation could not be recovered |
| `InvalidProblem` | `InvalidModel` | malformed input |

**The barrier method computes no infeasibility or unboundedness certificate.**
It has no homogeneous self-dual embedding, so a diverging iterate, or
complementarity converging while a residual does not, is *consistent with*
infeasibility or unboundedness but proves neither. It is therefore never
reported as `Infeasible` or `Unbounded`. Rerun with `--solver dual_simplex` for a
certificate.

## Crossover

The interior-point method converges to the analytic centre of the optimal face:
strictly inside every bound, with small but nonzero multipliers on inactive
constraints. This pipeline's postsolve is vertex-shaped — it returns a reduced
cost to an eliminated singleton row only when the variable sits *exactly* on the
bound that row produced. Measured on Netlib `adlittle`, the raw interior point
left 1.83 of reduced cost stranded on a bound that is not active in the original
model, and postsolve rightly withheld the duals.

For LPs, the orchestrator therefore follows an optimal barrier solve with
crossover, as production barrier codes do:

1. rank every column by distance-to-nearest-bound over multiplier magnitude,
   which complementarity separates by many orders of magnitude near optimality;
2. greedily select `m` linearly independent basic columns in that order, then
   complete with row logicals, which always succeeds;
3. warm-start the dual simplex from that basis for the cleanup pivots.

**The vertex is accepted only if the simplex reports `Optimal` and its objective
agrees with the interior optimum.** Otherwise the interior solution is returned
unchanged, so crossover can add a vertex but never make a result worse. It is on
by default (`SolverOptions::barrierCrossover`), applies to linear objectives
only, and runs only within the dense dual simplex's size limits.

## Polishing

Crossover needs a vertex, and a QP optimum need not be one. For both LPs and
QPs the solver therefore first tries **active-set polishing** at convergence:

1. mark a bound active when its slack is smaller than its multiplier — near
   optimality complementarity separates the two groups;
2. fix those variables at their bounds and solve the resulting
   equality-constrained KKT system, factorised with a tiny quasidefinite
   regularisation and then **refined against the unregularised matrix**, so the
   regularisation leaves no bias;
3. recover the bound multipliers from stationarity.

The result has exactly zero slack on active bounds and exactly zero multipliers
on inactive ones. It is **accepted only if** free variables stay in their boxes,
active multipliers have the right sign, both relative residuals meet tolerance,
and the objective is no worse than the interior one.

A one-shot identification is sometimes wrong. A failed attempt that reveals why
proposes a corrected set: a variable that left its box becomes active, one held
at a bound with a wrong-sign multiplier is released, and the solve retries, up to
20 rounds. Only the single **most violated** bound is activated per round. Adding
every violator at once overshoots: on `min x^2 - 4x, x + y = 1.5, 0 <= x <= 1,
y >= 0` with both variables free, `x = 2` and `y = -0.5` both leave their boxes,
but only `x` belongs at a bound, and fixing both makes the row infeasible.

Every attempt is verified in full, so a retry can only succeed on a genuine KKT
point. On degenerate LPs the free-set KKT system is often singular and polishing
declines; crossover is the LP tool there.

## Limitations

These are real, and are stated so the engine is not mistaken for a production
barrier code:

- **Serial.** No multithreading and no GPU. The factorisation is simplicial, not
  supernodal, so it gets no BLAS-3 blocking. Large-scale performance has not been
  established.
- **Ordering.** AMD's external-degree bound on a quotient graph with element
  absorption, but without supervariable detection or mass elimination.
- **No certificates** of infeasibility or unboundedness; see above.
- **Starting point** is a simple interior heuristic, not Mehrotra's
  least-squares start.
- **Crossover depends on the dual simplex**, which has no dual Phase 1. From a
  basis that is not exactly dual feasible — typical of degenerate problems — it
  stalls, and crossover declines. On Netlib it declines on `degen2` and
  `share2b`. Both still verify as optimal, but only through the raw interior
  point: its active slacks happen to clear the checker's 1e-6 activity
  threshold. That is fragile, and should not be read as robust vertex recovery.
  The dual Phase 1 on the unmerged `warm-start-integration-backup` branch is the
  fix.
- **Polishing's regularisation escalation is untested.** Escalating the
  polishing system's regularisation on an inertia failure fires on the Netlib
  LPs, but no accepted result depends on it and no test binds it; it is kept as
  defensive code.
- **Not selected automatically.** Automatic dispatch is unchanged; the barrier is
  opt-in with `--solver barrier` until benchmarks justify a default rule.

## Validation

```
ctest --test-dir build -R barrier
```

- `barrier_linalg_tests` — CSC assembly, AMD fill reduction (a 200x200 arrow
  matrix goes from 19,900 fill entries under the natural order to 199), LDL' on
  SPD and quasidefinite systems checked against dense partial pivoting, and the
  quasidefinite inertia (exactly `n` negative pivots).
- `barrier_solver_tests` — known optima with every result re-verified
  independently: a free variable, a convex QP, an equality QP with no bounds at
  all, a five-way degenerate vertex, coefficients spanning 1e-4 to 1e4,
  infeasible and unbounded problems reported without a false certificate, the
  inertia check firing on an indefinite leading block, and polishing landing
  exactly on a bound, refusing two deliberately wrong active sets, and
  correcting its way from the worst start to the right one.
- `barrier_pipeline` — through the orchestrator, by cross-engine agreement with
  the dual simplex (whose duals match HiGHS): objectives and every shadow price
  across minimisation/maximisation, `<=`/`>=`/equality/ranged rows and fixed
  variables, via both `solve()` and `solveReduced()`; QP against the analytic
  optimum and ADMM; refusal of a forced nonconvex QP; `LimitReached` rather than
  `Infeasible` on an infeasible model; crossover landing exactly on a vertex, and
  its acceptance gate refusing a vertex that disagrees with the interior optimum.
  Cross-engine dual agreement runs with crossover both on and off, because with
  it on an LP's duals come from the simplex and would hide an error in the
  barrier's own.

Each property above was confirmed to fail under a deliberately broken solver —
seventeen mutations, sixteen caught. The survivor is the polishing
regularisation escalation noted under Limitations.

### Results

Measured with the repository's benchmark harness: every solve in an isolated
process, single-threaded, 60 s limit, and every verdict from the **independent
checker** (`benchmarks/lib/verify.py`, frozen tolerances: feasibility 1e-6 abs +
1e-8 rel, normalised optimality 1e-6). `optimal_verified` means primal
feasibility, dual feasibility, complementarity and gap were all independently
confirmed — not that the solver said so. Instance sets are the frozen selections
already in the repository: none were added or swapped after seeing results.

**Netlib LP** (the 8 frozen instances; HiGHS 1.15.1 native, simplex):

| instance | dual simplex | PDLP | **barrier** | HiGHS | barrier iterations | barrier route |
|---|---|---|---|---|---|---|
| adlittle | ✅ | feasible | ✅ | ✅ | 13 | crossover, 1 pivot |
| afiro | ✅ | ✅ | ✅ | ✅ | 9 | polish + crossover, 0 pivots |
| blend | feasible | ✅ | ✅ | ✅ | 11 | crossover, 0 pivots |
| degen2 | ✅ | ✅ | ✅ | ✅ | 12 | interior point only |
| recipe | ✅ | ✅ | ✅ | ✅ | 9 | polish + crossover |
| sc50a | ✅ | ✅ | ✅ | ✅ | 10 | polish + crossover |
| sc50b | ✅ | feasible | ✅ | ✅ | 8 | polish + crossover |
| share2b | feasible | ✅ | ✅ | ✅ | 12 | interior point only |
| **verified** | 6/8 | 6/8 | **8/8** | 8/8 | 8–13 | |

On `degen2` the barrier is about 2.4x faster than our dual simplex and about
26x slower than HiGHS (0.58 s, 1.40 s and 0.022 s in the latest single run;
absolute wall times vary with machine load, the ratios have held across runs).
It peaked at 11.6 MB against HiGHS's 5.3 MB: factor fill, which the missing
supervariable detection likely worsens.

**Maros-Meszaros convex QP** (the 14-instance frozen smoke list in
`benchmarks/suites/qp.json`, SHA-256 verified; published objectives as ground
truth; OSQP 1.1.3), measured on `main` including the ADMM engine fixes merged
with PR #17:

| | ADMM (`qp`) | **barrier** | OSQP |
|---|---|---|---|
| optimal_verified | 12/14 | **14/14** | 14/14 |
| no point returned | 1 (`cvxqp3s`) | 0 | 0 |

`optimal_verified` is the independent checker's full KKT verdict, which is
stricter than the objective agreement that `run_suites.py` reports (13/14 for
ADMM). The difference is ADMM's `cvxqp1s`: its objective agrees to 4e-9 and
its gap is 4e-10, but one multiplier's sign violation is 1.02e-6, just above
the checker's frozen 1e-6 threshold -- a near-optimal answer, not a wrong one.
`cvxqp3s` is the ADMM engine's documented per-row-rho limitation.

Eleven barrier results were polished, three of them after 2–3 active-set
corrections. The remaining three (`hs51`, `hs52`, `genhs28`) have no finite
bounds at all: the Newton step is the exact KKT solve, and they finish in 1–2
iterations. On `cvxqp1s` the barrier took 0.007 s against 0.40 s for ADMM and
0.24 s for OSQP.
OSQP's memory figures (~49 MB) include its Python interpreter and are not
comparable.

These are small instances. They establish correctness and robustness on
standard, independently checked problems. They do not establish performance at
industrial scale.
