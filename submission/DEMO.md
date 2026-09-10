# SIH 2026 Demonstration Video

This document contains instructions, placeholders, and a suggested script sequence for the SIH 2026 project demonstration video.

---

## Video Links

Upload the recording to YouTube (set as Unlisted or Public) or Google Drive, then add the link below:

- **Primary Video Link (YouTube / Google Drive):**
  `[PASTE_DEMO_VIDEO_LINK_HERE]`

- **Backup Mirror Link:**
  `[PASTE_BACKUP_LINK_HERE]`

> Note: If using Google Drive, ensure link sharing permissions are set to "Anyone with the link can view".

---

## Suggested Demo Script & Flow (3–5 Minutes)

### 1. Introduction & CLI Startup (~30s)
- State the project title and Problem Statement focus.
- Show the built binary in `./build/optimsolver`.
- Run the top-level help command to display the welcome banner, robot mascot, and command list:
  ```bash
  ./build/optimsolver --help
  ```

### 2. Solving a Continuous Linear Program (~45s)
- Run a baseline solve on a sample linear programming model:
  ```bash
  ./build/optimsolver solve tests/cli/simple_lp.mps
  ```
- Highlight the solve dashboard card:
  - Model dimensions vs. reduced dimensions
  - Active solver engine selected by the dispatcher
  - Solve status (`Optimal`), objective value, and iterations
  - Availability of shadow prices and reduced costs

### 3. Presolve Reduction & Solution Export (~60s)
- Solve an instance that exercises presolve reductions and exports the solution vector:
  ```bash
  ./build/optimsolver solve tests/cli/presolve_reduction.mps --output /tmp/solution.txt
  ```
- Explain the pipeline behavior:
  - The original problem has 3 variables; presolve detects that variable `X3` is fixed and reduces the model to 2 variables before calling the solver.
  - The engine solves the 2-variable reduced model.
  - Postsolve reconstructs all 3 original variables and calculates shadow prices and reduced costs.
- Inspect the exported solution file:
  ```bash
  cat /tmp/solution.txt
  ```
  Note that `X3` is correctly restored to its fixed value, and dual multipliers are included.

### 4. Engine Override via CLI Flag (~45s)
- Demonstrate overriding the automatic dispatcher to select a specific engine:
  ```bash
  ./build/optimsolver solve tests/cli/simple_lp.mps --solver pdlp
  ```
- Point out the solver switch to `pdlp` in the terminal dashboard.

### 5. Automated Test Suite Execution (~30s)
- Run the CTest suite to demonstrate automated verification across unit, cascade, and integration tests:
  ```bash
  ctest --test-dir build --output-on-failure
  ```
- Highlight that all registered test targets execute and pass cleanly.

### 6. Wrap Up (~30s)
- Summarize the modular pipeline architecture (single presolve, solver dispatch, invertible postsolve with dual reconstruction).
- Mention roadmap directions, including `.lp` file parsing support and additional presolve reduction rules.
