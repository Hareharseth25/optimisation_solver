# Architecture Notes & Pipeline Design

This document details the software architecture, dataflow pipeline, module contracts, and design decisions of `optimisation_solver`.

---

## 1. Pipeline Overview

The solver processes models through a sequential, modular pipeline:

```text
       MPS File
          ↓
       Model IR  (src/mps, src/model)
          ↓
       Validation  (model.validate())
          ↓
       Original Classification  (solver::classify())
          ↓
       Presolve (Once)  (src/presolve) ── logs PresolveResult ──┐
          ↓                                                    │
       Reduced Model                                           │
          ↓                                                    │
       Solver Dispatch & solveReduced()                        │
       ├── PDLP (pdlp_engine/)                                 │
       ├── Dual Simplex (milp_engine/)                         │
       ├── Branch-and-Cut (milp_engine/)                       │
       └── QP (qp_engine/)                                     │
          ↓                                                    │
       Engine Normalization                                    │
          ↓                                                    │
       Reduced-Space SolveResult                               │
          ↓                                                    │
       Postsolve (Once)  (src/postsolve) <─────────────────────┘
          ↓
       Original-Space SolveResult (Primal & Dual Validation)
```

Each module has a clearly defined interface and data ownership boundary.

---

## 2. Pipeline Execution Stages

### Stage 1: MPS Ingestion & Model IR (`src/mps/`, `src/model/`)
- `mps_reader` parses fixed-format and free-format `.mps` files into memory.
- `model::Model` is the internal intermediate representation (IR), storing:
  - Variables with bounds, types (`Continuous`, `Integer`, `Binary`), and names.
  - Linear constraints in sparse representation, with lower and upper row bounds.
  - Objective offset, linear objective terms, and quadratic terms ($q_{ij} x_i x_j$, where $q_{ij}$ is the direct coefficient without an implicit $1/2$ factor).
- `model.validate()` verifies that variable and constraint bounds are structurally sound ($lb \le ub$) and that all sparse row indices point to valid variables.

### Stage 2: Original Model Classification (`src/solver/classifier.cpp`)
- `solver::classify(const model::Model&)` analyzes the **original** problem formulation:
  - Determines Problem Class: `LP` (linear continuous), `QP` (convex quadratic objective with linear constraints), or `MILP` (linear with integer or binary variables).
  - Inspects whether quadratic objective terms exist and checks for integrality requirements.
  - Generates a `solver::Classification` containing problem class flags and the default solver engine.

### Stage 3: Invertible Presolve Pipeline (`src/presolve/`)
- Executes reduction passes on the model to reduce rows and columns before solving:
  - **Fixed variable elimination:** Detects variables where $lb_i = ub_i$, logs their values into `PresolveResult::fixedVariables`, and removes them from the model.
  - **Singleton equality rows:** Identifies rows containing a single non-zero entry $a \cdot x_i = b$, computes $x_i = b/a$, updates bounds, records the transformation, and eliminates the row and variable. Both positive and negative coefficients are handled.
  - **Singleton inequality rows (bound tightening):** Uses single-variable rows to tighten variable bounds and records the provenance (which constraint tightened which bound) in `PresolveResult::boundProvenance` so dual multipliers can be attributed correctly during postsolve.
  - **Redundant constraints:** Computes minimum and maximum constraint activity based on variable bounds; if $[\text{activity}_{\min}, \text{activity}_{\max}] \subseteq [lb, ub]$, the row cannot be violated and is removed.
- **Single-Presolve Invariant:** Presolve is executed exactly once on the original model. Re-running presolve inside solver engines or dispatchers is prohibited because postsolve reconstruction requires the exact transformation mapping from the initial reduction.

### Stage 4: Engine Selection & `solveReduced()` (`src/solver/orchestrator.cpp`, `src/solver/dispatcher.cpp`)
- High-level orchestration provides two APIs:
  - **`solver::solve(model, options)`:** Complete pipeline: validates, classifies the original model, runs presolve once, calls `solveReduced()`, normalizes engine output, runs postsolve once, and returns the original-space solution.
  - **`solver::solveReduced(presolvedModel, originalClassification, options)`:** Dispatches directly to the selected solver engine on the reduced model, using the `Classification` of the original model.
- **Dispatcher Contract:** The dispatcher inspects the **original** classification (to know if quadratic terms were present originally) while inspecting the **reduced** model for runtime integrality (to detect if presolve eliminated all integer variables, turning a MILP into an LP).
- **Engine Selection:**
  - If the user provides `--solver <name>`, the requested engine is used (`pdlp`, `dual_simplex`, `branch_and_cut`, `qp`).
  - Otherwise, the problem is routed based on problem class: pure continuous LPs default to `dual_simplex` or `pdlp`, integer models route to `branch_and_cut`, and quadratic models route to `qp`.

### Stage 5: Solver Result Normalization (`src/solver/orchestrator.cpp`)
- Different solver engines have distinct internal status enums (`PdlpStatus`, `DualSimplexStatus`, `MilpStatus`, `QpStatus`).
- The orchestrator normalizes these into a uniform `solver::SolveStatus` (`Optimal`, `Infeasible`, `Unbounded`, `LimitReached`, `NumericalFailure`, `InvalidModel`).
- Engine results are converted into a `solver::SolveResult` before being passed to postsolve.

### Stage 6: Postsolve Reconstruction & Validation (`src/postsolve/`)
- `postsolve::Postsolver` reverses the transformations recorded in `PresolveResult` to map reduced-space results back into original model coordinates:
  - **Primal Reconstruction:** Maps reduced variable values back to their original variable indices using `presolvedToOriginalVar`, and restores fixed variables from `fixedVariables`.
  - **Non-Finite Detection:** Rejects solutions containing `NaN` or `±Inf`, returning `PostsolveStatus::BoundViolation`.
  - **Objective Re-evaluation:** Computes the objective value directly from reconstructed primal variables using the original objective terms, catching any offset or scaling discrepancy.
  - **Primal Feasibility Verification:** Computes maximum bound residual and maximum constraint residual against original bounds.
  - **Dual Reconstruction:** Recovers original-space constraint duals (shadow prices) and variable reduced costs:
    - Reduced costs are calculated via $d_j = \nabla_j f(x^*) - \sum_i a_{ij} y_i$.
    - Constraint duals from presolved rows are mapped back to their original constraint indices.
    - Multipliers from tightened bounds are attributed back to the source singleton rows using the recorded bound provenance history.
  - **Dual Optimality Checks:** Checks stationarity, dual sign feasibility, and complementary slackness against the original constraints (`maxDualResidual`).
  - **Fail-Closed Dual Behavior:** If reduced-space duals were not supplied by the engine, or if any transformation step cannot be reliably inverted, `dualsAvailable` is set to `false` and a specific `dualsUnavailableReason` is recorded, rather than emitting incorrect or unmapped values.

### Observability: `SolveResult` vs `SolveReport` (`include/solver/solve_report.h`)
- **`SolveResult`** is the solve's outcome: status, original-space point and duals, engine provenance.
- **`SolveReport`** describes how that outcome was reached: the classification used, a presolve summary (counts, nonzeros, transformations by type), the dispatch decision, reduced- and original-space validation residuals, and per-stage `steady_clock` timings.
- The report is optional: `solver::solve(model, options, &report)` (and the same for `solveReduced`). Existing two-argument calls are unchanged, and asking for a report never changes the `SolveResult`.
- The report is filled in place by the **same** run. It must never call `classify()`, `Presolver::run()`, `dispatch()`, an engine or postsolve a second time to obtain its data. If a later stage needs more information, record it where the pipeline already computes it.
- A section left as `std::nullopt`, or a stage time below zero, means that stage did not run. `stageSeconds.total` always equals `SolveResult::solveSeconds`.
- Consumers: the CLI requests the report from its one `solver::solve()` call and feeds it to the terminal summary, `--verbose`, and the `optimsolver.solve.v1` JSON record (`classification`, `presolve`, `dispatch`, `validation`, `stage_seconds`). The JSON record is the machine-readable contract; nothing should parse the terminal output.

### Product architecture

```text
                     KAIRO
                       │
              ┌────────┴────────┐
              │                 │
         KAIRO Core          Interfaces
              │                 │
        ┌─────┴─────┐      ┌────┴─────┐
        │           │      │          │
     Engines   solve() API Desktop    CLI
                          (Qt 6)   (optimsolver)
                           │
                 ┌─────────┼─────────┐
               macOS    Windows    Linux
```

- **One solver implementation:** KAIRO Core (model IR, MPS reader, classification, presolve, dispatcher, engines, postsolve, validation).
- **One execution path:** `solver::solve(model, options, &report)`. Both interfaces call it once per run.
- **One structured record:** `optimsolver.solve.v1`, written only by `cli::writeJsonReport` (the `solve_report_json` library) from that call's `SolveResult` and `SolveReport`.
- **One interpretation path:** the desktop's `desktop/src/record` readers. A live run, a saved run and an imported run all go through them.
- There is **no web layer**. No browser, HTTP server or localhost port is involved, and building, running or packaging KAIRO needs no Python or JavaScript. Some optional core test scripts use Python. An earlier web Explorer prototype has been retired. Its real record fixtures now live in `desktop/tests/fixtures/records`.

### Solve-record contract

Any interface is a **consumer** of solver runs, never a participant in them.

```text
   model + SolverOptions
            │
            ▼
  solver::solve(model, options, &report)      ← single source of truth
            │                     │
      SolveResult            SolveReport
      (outcome)              (how it ran)
            └─────────┬───────────┘
                      ▼
   cli::writeJsonReport()  [solve_report_json]   pure mapping, runs no stage
                      │
                      ▼
          optimsolver.solve.v1 record
            ┌─────────┴──────────────┐
            ▼                        ▼
   optimsolver solve --json     KAIRO Desktop (in-process,
   (CLI, process boundary)      links solve_report_json)
```

1. Interfaces contain no solver logic.
2. Interfaces never invoke individual stages (`classify`, `Presolver::run`, `dispatch`, engines, postsolve). They request one solve.
3. Nothing parses CLI terminal output. The terminal text is for people; the JSON record is the machine contract.
4. The solver core is the single source of truth. Every value in the record comes from the one `solver::solve()` call; the writer reads it and computes nothing.
5. `SolveResult` is the outcome: status, point, duals, engine provenance, integrality verdict.
6. `SolveReport` is the execution observability: classification, presolve summary, dispatch decision, stage timings, validation residuals.
7. `optimsolver.solve.v1` is the serialization contract. It changes only additively; a breaking change would need a new schema id.

Not part of the contract:
- **Run identifiers.** A run is identified by `instance.sha256`, `settings` and `solver.commit`/`build_type`. The desktop gives each saved run a local UUID.
- **Wall-clock timestamps.** Only durations are recorded.

`tests/api/test_solve_contract.cpp` exercises the contract without the CLI, and `tests/cli/test_solve_report_json.py` exercises it through the process.

Every model the CLI or desktop accepts produces a record, including one the MPS reader rejects. That record has `invalid_model`, the reader's message in `termination.message`, and `instance.variables`/`constraints` set to null. Only an option value KAIRO rejects before reading the model leaves no record.

### KAIRO Desktop (`desktop/`)

A native **Qt 6 Widgets / C++17** application for macOS, Windows and Linux. It works locally and offline: it needs no browser or web server, makes no network calls and has no account.

- `desktop/src/core/KairoSession` is the only code that touches the solver. It calls the core in-process (`mps::MpsReader` → `Model::validate()` → `solver::solve(model, options, &report)`) and has `cli::writeJsonReport` serialize the result in memory. The per-run order is the CLI's, so unreadable or invalid models produce the same record shape (no report, `dispatch: null`).
- `desktop/src/record` (Qt Core only) turns a record into what is shown:
  - `RecordReaders`: result, evidence, pipeline, model analysis, presolve impact, dispatch;
  - `Comparison`: two runs side by side.

  They are pure readers:
  - the only arithmetic is the presolve reduction percentage and stated differences between two recorded values;
  - **`dispatch.reason` is shown verbatim**;
  - the selected and executed engines stay separate;
  - the pseudo-engines (`infeasible`, `trivial`, `unsupported`) are outcomes;
  - engine time is "execution" only when an engine executed, and otherwise "engine path".
- **Trust wording** never exceeds the record:
  - KAIRO's own validation (`validation.*`, `self_reported.*`) is the only source of *Checked*. Claims without a recorded certificate, bound or gap read *Reported by engine* or *Proved by presolve*.
  - `integrality_respected: false` on an integer model turns `optimal` into "Optimal for the continuous relaxation", and an integer optimum is "Optimal — according to the solver".
  - With no solution point, nothing is shown as validated.
  - The words "certified", "guaranteed" and "proven optimal" are never used.
- `desktop/src/history/RunStore` keeps saved runs as local JSON files (`QStandardPaths`, format `kairo.desktop.saved_run.v1`): the unchanged record plus a few local facts, and never the model text.
  - **Export** writes the record unchanged.
  - **Import** checks the record's shape and stores it marked imported. It never runs the solver or anything else.
- **Comparison** treats runs as the same model only when their recorded input SHA-256 values match. With different hashes, objectives, residuals and presolve reductions are not compared. Values are stated, never ranked.
- `desktop/src/ui` holds widgets only. Build, packaging and platform status are in `desktop/README.md`.

**Live progress and cancellation (not implemented).** `solver::solve()` is synchronous and reports only after it finishes, so the desktop shows only "Running KAIRO…".
- Stage-level events (classified, presolved, dispatched, engine started or finished, validated) need no redesign: an optional observer in `SolverOptions`, invoked at the orchestrator's existing stage boundaries, as the report already is.
- Engine-level progress (iterations, gap, incumbents) is the real obstacle. None of the affine engines (PDLP, dual simplex, barrier, ADMM, branch-and-cut, MIQP) exposes an iteration hook today. Only the NLP solver has an iteration callback.
- There is no cancellation API besides the time limit.

---

## 3. Implemented Solver Engines

### 1. PDLP Engine (`pdlp_engine/`)
- **Algorithm:** Primal-Dual Hybrid Gradient (PDHG) first-order method for linear programming.
- **Components:**
  - `pdhg_kernel.cpp`: Core iterate updates with operator extrapolation.
  - `step_controller.cpp`: Adaptive step-size adjustments (Malitsky-Pock and line-search checks).
  - `preconditioner.cpp`: Ruiz diagonal equilibration and Pock-Chambolle scaling.
  - `restart_controller.cpp`: Restarts based on normalized duality gap metrics.
  - `termination.cpp`: Primal and dual feasibility and duality gap convergence criteria.

### 2. Dual Simplex Solver (`milp_engine/src/dual_simplex_solver.cpp`)
- **Algorithm:** Tableau-based dual simplex algorithm for continuous linear programs.
- **Functionality:**
  - Maintains dual feasibility while iterating toward primal feasibility.
  - Delivers basic solutions (vertex points).
  - Serves as the subproblem LP relaxation solver at each node of the branch-and-cut tree.

### 3. Branch-and-Cut Engine (`milp_engine/`)
- **Algorithm:** Branch-and-bound search with dynamic cutting plane separation for Mixed-Integer Linear Programs (MILP).
- **Components:**
  - `branch_and_bound.cpp`: Priority queue node selection and tree search management.
  - `gomory_cuts.cpp`: Generates Gomory fractional cuts from fractional simplex tableau rows to separate non-integer LP relaxation points.
  - `branching_rules.cpp`: Variable selection rules (most-fractional branching).
  - `heuristics.cpp`: Primal feasibility pump and rounding heuristics to find integer feasible solutions during tree search.

### 4. QP Engine (`qp_engine/`)
- **Algorithm:** Alternating Direction Method of Multipliers (ADMM) for convex Quadratic Programming.
- **Objective Mathematical Convention:**
  - Model IR stores quadratic objective terms as:
    $$f(x) = \text{offset} + \sum c_i x_i + \sum q_{ij} x_i x_j$$
    where $q_{ij}$ is the direct coefficient of $x_i x_j$ without an implicit $1/2$ factor.
  - The QP adapter (`qp::fromModel`) converts this to standard quadratic form:
    $$\min \frac{1}{2} x^T P x + q^T x + \text{offset}$$
    with diagonal entries $P_{ii} = 2 q_{ii}$ and off-diagonal entries $P_{ij} = P_{ji} = q_{ij}$ (undoubled, since $\frac{1}{2}(P_{ij} + P_{ji}) = q_{ij}$). Maximization negates both $P$ and $q$.
- **Components:**
  - `admm_solver.cpp`: Alternating minimization over primal variables and constraint slacks with dual multiplier updates.
  - `kkt_solver.cpp`: Factorization of augmented KKT linear systems.
  - `scaling.cpp`: Matrix equilibration and adaptive penalty parameter ($\rho$) updating.

---

## 4. Key Architectural Decisions

### Decoupling Presolve and Postsolve
Presolve and postsolve are completely decoupled:
- Presolve only transforms `model::Model` and outputs an audit log (`PresolveResult`).
- Postsolve takes that log and the reduced solution vector to reconstruct original-space coordinates.
This separation allows presolve rules to be tested independently from solver engines, and allows engines to solve the reduced problem without needing any knowledge of how presolve occurred.

### Scaling Location & Preserving Presolve Reductions
A frequent source of numerical error in mathematical programming pipelines is redundant or conflicting matrix scaling.
- **Presolve does model reduction, not matrix scaling.** It eliminates variables and rows, tightens bounds, and preserves integer coefficient properties for MILP cuts.
- **Numerical engines handle their own scaling.** PDLP performs Ruiz equilibration and diagonal preconditioning inside `pdlp_engine/src/preconditioner.cpp`. The QP engine handles its own matrix scaling internally.

### Single-Presolve Invariant
The orchestrator classifies the original model once, runs presolve once, and passes the reduced model into `solveReduced()`. Nesting or repeating presolve passes is strictly forbidden because postsolve metadata relies on the exact index mappings established during the initial presolve.

---

## 5. Current Implementation vs. Planned Work

| Subsystem | Implemented in Current Codebase | Planned Work |
| :--- | :--- | :--- |
| **Input Format** | MPS (Fixed & Free format) | LP format (`.lp`) |
| **Model IR** | Linear, Quadratic terms ($q_{ij} x_i x_j$), Continuous/Integer/Binary types | Conic constraints (SOCP) |
| **Presolve** | Fixed variables, Singleton equality/inequality, Bound tightening with provenance, Redundant rows | Variable substitution, Binary probing, Duplicate row/column detection |
| **Engines** | PDLP (First-Order LP), Dual Simplex, Barrier (primal-dual interior point, LP/QP, with crossover and active-set polishing), Branch-and-Cut (MILP), ADMM (QP) | Parallel/supernodal barrier factorisation; homogeneous self-dual embedding for infeasibility certificates |
| **Postsolve** | Full primal reconstruction, Objective re-evaluation, Dual reconstruction (shadow prices & reduced costs), Bound provenance validation, Fail-closed error handling | Support for dual reconstruction through variable substitution passes |
| **CLI & UX** | Interactive menu, Batch solve mode, Mascot banner, Solve dashboard, Solution export, TTY detection, CMake install | Real-time solve progression streaming |

## Smooth nonlinear models

`nlp::Problem` and `nlp::Model` use a separate nonlinear pipeline through the
`solver::solve(problem, initial, nlp::Options)` overload. This path bypasses
linear presolve/postsolve, reuses `qp_engine` for elastic SQP subproblems, and
returns `solver::NlpSolveResult` with shared classification/engine metadata,
first-order stationarity and original-unit KKT
residuals. The affine `SolveResult::Optimal` contract is unchanged. See the
[NLP design and numerical limitations](../nlp_engine/README.md) for the
architecture decision, expression AD, globalization, scaling and validation.

The shared CLI parser routes `.nlp` files from `solve`, or accepts an explicit
`solve-nlp` command. Interactive model state stores the nonlinear input snapshot
separately and uses the same orchestrator/report writer. Nonlinear JSON uses the
`optimsolver.nlp.v1` schema; affine reports retain their existing schema. Engine
mismatches are rejected before affine presolve or nonlinear evaluation.
