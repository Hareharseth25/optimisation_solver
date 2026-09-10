# SIH 2026 Demonstration Video

This document outlines the demonstration video script, terminal workflow, and video links for SIH 2026.

---

## Project Information

- **Organization:** Mangalore Refinery and Petrochemicals Limited (MRPL)
- **Problem Statement ID:** SIH26119
- **Problem Statement Title:** Indigenous GPU-Accelerated Optimization Solver (Sovereign Alternative to Express / CPLEX)
- **Category:** Software (Smart Automation)

### Video Links

*Upload the recording to YouTube (Unlisted or Public) or Google Drive, then update the links below:*

- **Primary Video Link (YouTube / Google Drive):**
  *Video link will be added upon final recording.*
- **Backup Mirror Link:**
  *Backup link will be added upon final recording.*

---

## Demonstration Script & Video Flow (3–5 Minutes)

### 1. Introduction (~30s)
- State the project title and MRPL Problem Statement ID (SIH26119).
- Explain the motivation: creating a sovereign, open-source, modular optimization solver for LP, MILP, and QP to reduce reliance on expensive closed-source commercial tools.
- Highlight the repository architecture and clean separation of concerns.

### 2. Interactive Terminal Interface (~60s)
- Launch the interactive mode:
  ```bash
  ./build/optimsolver
  ```
- Walk through the terminal interface:
  - Point out the robot mascot banner, home screen card, and problem families (LP, QP, MILP).
  - Select Option `1` (Open MPS Model).
  - Enter `tests/cli/simple_lp.mps`.
  - Highlight the active **CURRENT MODEL** context card and the detailed **Model Information** screen.
  - Select `[1]` to solve the current model.
  - Review the **SOLVE RESULT** card: status (`✓ OPTIMAL`), objective value, solve time, and primal, integrality, and dual feasibility checks.
  - Return to the main menu with current model context preserved.

### 3. Non-Interactive Command-Line Solve (~45s)
- Exit or open a clean terminal to demonstrate batch mode:
  ```bash
  ./build/optimsolver solve tests/cli/simple_lp.mps
  ```
- Demonstrate forcing a different engine via `--solver`:
  ```bash
  ./build/optimsolver solve tests/cli/simple_lp.mps --solver pdlp
  ```
- Highlight that the dispatcher honours the user's engine override.

### 4. Presolve Reduction & Solution Export (~60s)
- Solve a problem that exercises presolve reductions and exports the solution:
  ```bash
  ./build/optimsolver solve tests/cli/presolve_reduction.mps --output /tmp/solution.txt
  ```
- Explain the pipeline behavior:
  - Original problem has 3 variables; presolve detects that variable `X3` is fixed and reduces the model to 2 variables before calling the solver.
  - The solver runs on the reduced 2-variable problem.
  - Postsolve reconstructs all 3 original variables, re-evaluates the objective, and verifies feasibility against original constraints.
- Inspect the output file:
  ```bash
  cat /tmp/solution.txt
  ```
  Show that `X3` is correctly restored to its fixed value of 5, along with constraint duals and reduced costs.

### 5. Automated Verification & Testing (~30s)
- Run CTest to demonstrate automated verification across all test targets:
  ```bash
  ctest --test-dir build --output-on-failure
  ```
- Highlight 100% passing tests across unit algorithms, cascades, postsolve dual checks, and integration tests.

### 6. Wrap Up & Roadmap (~30s)
- Conclude with the roadmap:
  - Adding GPU/CUDA accelerated linear algebra kernels for large-scale PDLP and QP solves.
  - Ingesting LP format (`.lp`) files.
  - Multi-threaded branch-and-bound search.
