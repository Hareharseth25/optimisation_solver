# Optimisation Solver

An open-source mathematical optimization solver written in C++17 for linear programming (LP), mixed-integer linear programming (MILP), and convex quadratic programming (QP).

The solver parses an MPS model, constructs an internal model representation, validates problem consistency, runs presolve reductions, dispatches the reduced problem to a solver engine, and reconstructs both primal and dual solutions in original coordinates.

---

## Project Information

> The SIH problem statement details and final team information will be added before submission.

- **Problem Statement ID:** `[SIH2026-PS-ID-PLACEHOLDER]`
- **Problem Statement Title:** `[SIH2026-PS-TITLE-PLACEHOLDER]`
- **Category:** Software
- **Repository:** `https://github.com/RaghavGupta2910/optimisation_solver`

---

## Problem & Architectural Goals

Mathematical programming solvers often tightly couple input parsing, presolve transformations, numerical kernels, and postsolve steps. When coordinate systems and reduction histories are not tracked cleanly, presolve transformations become difficult to reverse, and solution reconstruction can fail or produce coordinate mismatches.

The architectural goal of this project is a modular optimization pipeline with clean boundaries:
1. Parse problem definitions into an explicit in-memory model representation.
2. Validate structural integrity and bound feasibility before solving.
3. Classify the original problem formulation to determine problem class and routing options.
4. Run presolve reductions exactly once while logging all transformations in invertible metadata.
5. Solve the reduced problem using the selected engine.
6. Postsolve the reduced solution back into the original coordinate space, reconstructing primal values, constraint duals (shadow prices), and reduced costs with residual verification.

---

## What Is Implemented

- **MPS Reader (`src/mps/`):** Reads fixed-format and free-format `.mps` files, supporting standard sections (`ROWS`, `COLUMNS`, `RHS`, `RANGES`, `BOUNDS`, `QUADOBJ`, `QMATRIX`).
- **Model IR (`src/model/`):** In-memory model representation storing variable bounds, variable types (`Continuous`, `Integer`, `Binary`), sparse linear constraint matrices, and quadratic objective terms ($q_{ij} x_i x_j$). Structural validation verifies bound sanity ($lb \le ub$) and matrix index bounds.
- **Presolve Pipeline (`src/presolve/`):** A single-pass reduction pipeline that records all changes into invertible metadata:
  - Fixed variable elimination ($lb = ub$)
  - Singleton equality row elimination ($a x_i = b$)
  - Singleton inequality bound tightening with provenance tracking
  - Redundant constraint removal based on row activity intervals
- **Solver Engines:**
  - **PDLP (`pdlp_engine/`):** First-order Primal-Dual Hybrid Gradient (PDHG) algorithm for linear programs, with adaptive step-size control, normalized duality gap restart checks, and Ruiz diagonal preconditioning.
  - **Dual Simplex (`milp_engine/src/dual_simplex_solver.cpp`):** Tableau-based dual simplex implementation for continuous linear programs, providing basic solutions and serving as the relaxation solver during branch-and-bound.
  - **Branch-and-Cut (`milp_engine/`):** MILP solver combining a branch-and-bound search tree with Gomory fractional cut generation from tableau rows, branching rules, and primal rounding heuristics.
  - **QP Engine (`qp_engine/`):** Alternating Direction Method of Multipliers (ADMM) solver for convex quadratic programs with linear constraints and adaptive penalty parameter updates.
- **Postsolve & Dual Reconstruction (`src/postsolve/`):** Reconstructs original-space solutions from reduced-space solver output:
  - Primal reconstruction: Maps presolved indices back to original indices and restores eliminated fixed variables.
  - Objective reconstruction: Evaluates the original objective directly from reconstructed primal coordinates.
  - Dual reconstruction: Recovers original constraint duals (shadow prices) and variable reduced costs by walking transformation provenance logs in reverse.
  - Provenance validation: Verifies bound-tightening histories before attributing dual multipliers to source constraints.
  - Dual residual checks: Verifies stationarity, dual sign feasibility, and complementary slackness against original constraints before publishing duals.
  - Fail-closed behavior: If reduced-space duals are missing or any transformation cannot be safely inverted, duals are marked unavailable with a specific reason rather than returning invalid or unmapped values.
  - Non-finite validation: Explicitly detects and rejects non-finite primal values (`NaN`, `±Inf`).
- **CLI (`cli/`):** Terminal interface (`optimsolver`) with command-line argument parsing, problem dimension reporting, mascot banner, solver override flags, and solution file export.

---

## Pipeline Architecture

```text
    MPS File
       ↓
    Model IR
       ↓
    Validation
       ↓
    Classify Original Model
       ↓
    Presolve (Once) ─── logs metadata ───┐
       ↓                                 │
    Reduced Model                        │
       ↓                                 │
    solveReduced()                       │
       ↓                                 │
    Solver Engine                        │
       ↓                                 │
    Reduced-Space Result                 │
       ↓                                 │
    Postsolve (Once)  <──────────────────┘
       ↓
    Original-Space Solution (Primal + Dual Validation)
```

1. **MPS Parsing & Model IR:** Reads the input file into `model::Model`.
2. **Validation:** Checks that variable bounds and constraint bounds are consistent ($lb \le ub$) and matrix indices are within valid ranges.
3. **Original Model Classification:** Inspects the original model for integer variables and quadratic terms to determine problem class (`LP`, `MILP`, `QP`).
4. **Presolve (Run Once):** Reduces model dimensions and records all variable fixes, singleton eliminations, bound changes, and row removals into `PresolveResult`.
5. **Reduced Solve (`solveReduced`):** Dispatches the reduced model and original classification to the chosen engine, returning reduced-space primal values and dual multipliers.
6. **Postsolve (Run Once):** Reconstructs original variable values and constraint duals using the transformation log, then validates primal feasibility and optimality residuals against the original model.

See [docs/architecture.md](docs/architecture.md) for detailed module contracts and design notes.

---

## Repository Structure

```text
optimisation_solver/
├── README.md                      # Project overview and instructions
├── SUBMISSION_GUIDE.md            # SIH 2026 checklist and verification steps
├── LICENSE                        # MIT License
├── CMakeLists.txt                 # CMake build configuration
│
├── submission/
│   ├── PRESENTATION.md            # Slide deck link and presentation outline
│   └── DEMO.md                    # Demonstration video link and script
│
├── docs/
│   └── architecture.md            # Technical architecture notes and design decisions
│
├── assets/
│   └── screenshots/
│       └── README.md              # Screenshot plan and conventions
│
├── include/                       # Public headers (model, mps, presolve, postsolve, solver)
├── src/                           # Core implementation (MPS parser, presolve, postsolver, solver)
├── cli/                           # Command-line interface application
├── pdlp_engine/                   # PDHG first-order linear programming solver
├── milp_engine/                   # Dual simplex and branch-and-cut MILP solver
├── qp_engine/                     # ADMM convex quadratic programming solver
├── benchmark_model/               # Sample models for benchmarking
├── tests/                         # Unit, integration, and pipeline test suites
└── tools/                         # Utility scripts
```

---

## Build Instructions

### Prerequisites
- C++17 compatible compiler (GCC 9+ or Clang 10+)
- CMake 3.16 or newer
- Make or Ninja build system

### Compile
```bash
git clone https://github.com/RaghavGupta2910/optimisation_solver.git
cd optimisation_solver

cmake -S . -B build
cmake --build build -j
```

The compiled executable is generated at `./build/optimsolver`.

---

## Running the Solver

### Basic Usage
```bash
# Display help and usage information
./build/optimsolver --help

# Solve an MPS model with automatic solver selection
./build/optimsolver solve tests/cli/simple_lp.mps
```

### Options
```bash
# Select a specific solver engine (pdlp, dual_simplex, branch_and_cut, qp)
./build/optimsolver solve tests/cli/simple_lp.mps --solver dual_simplex

# Impose a time limit in seconds
./build/optimsolver solve tests/cli/simple_lp.mps --time-limit 30.0

# Export the solution to a file
./build/optimsolver solve tests/cli/simple_lp.mps --output solution.txt
```

---

## Testing

The project uses CTest for automated testing:

```bash
ctest --test-dir build --output-on-failure
```

The test suites cover:
- Presolve reduction rules and cascading reductions (`PresolveBatch2`, `PresolveCorrectness`, `PresolveTransformationMetadata`, `PresolvePipelineCascades`, `PresolveAdversarial`)
- Postsolve primal reconstruction, non-finite handling, and metadata checks (`PostsolveTests`, `test_presolve_metadata`)
- Dual reconstruction, shadow prices, reduced costs, bound provenance, and fail-closed checks (`PostsolveDual_*`)
- Postsolve pipeline integration across engines and problem classes (`PostsolveIntegration_*`)
- Dual simplex tableau pivoting and ratio tests (`test_dual_simplex`)
- Branch-and-bound tree search, Gomory cuts, branching rules, heuristics, and enumeration (`test_branch_and_bound`, `test_gomory_cuts`, `test_branching`, `test_heuristics`, `test_milp_enumeration`)
- PDLP adapter and step-size control (`test_pdlp_adapter`)
- QP convention, objective sense, and offset handling (`test_qp_convention`)
- End-to-end solve pipelines, MPS parsing, classifier/dispatcher routing, and CLI flags (`test_end_to_end`, `test_mps_reader`, `test_classifier_dispatcher`, `test_cli`)

---

## Current Limitations & Roadmap

- **Additional Input Formats:** The solver currently ingests `.mps` files. Adding a `.lp` format parser is planned.
- **Additional Presolve Passes:** Linear variable substitution, probing on binary variables, and duplicate row/column detection are planned.
- **Parallel Branch-and-Bound:** The branch-and-cut tree search is currently single-threaded.
- **Barrier Methods:** An infeasible interior-point solver for continuous linear and quadratic programs is planned for future work.

---

## Team

> The SIH problem statement details and final team information will be added before submission.

| Name | Role | Focus Area |
| :--- | :--- | :--- |
| `[Team Member 1]` | Team Lead | Architecture, pipeline orchestration, CLI |
| `[Team Member 2]` | Developer | PDLP first-order LP solver |
| `[Team Member 3]` | Developer | Dual simplex & branch-and-cut MILP solver |
| `[Team Member 4]` | Developer | ADMM convex QP solver |
| `[Team Member 5]` | Developer | Presolve reductions & transformation tracking |
| `[Team Member 6]` | Developer | Postsolve reconstruction & solution validation |

---

## Deliverables & Documentation Links

- **Architecture Documentation:** [docs/architecture.md](docs/architecture.md)
- **Submission Checklist:** [SUBMISSION_GUIDE.md](SUBMISSION_GUIDE.md)
- **Presentation Deck:** [submission/PRESENTATION.md](submission/PRESENTATION.md)
- **Demo Video:** [submission/DEMO.md](submission/DEMO.md)
- **Screenshots:** [assets/screenshots/README.md](assets/screenshots/README.md)
- **License:** [MIT](LICENSE)
