# SIH 2026 Presentation

This document tracks the slide deck outline and presentation link for the Smart India Hackathon (SIH) 2026.

---

## Slide Deck Links

- **Local Presentation File:** Place the final file in this directory and update the link below:
  - `[Final Presentation PPT](./OptimisationSolver_Presentation.pptx)`
  - `[Final Presentation PDF](./OptimisationSolver_Presentation.pdf)`
- **Cloud Backup Link (Google Drive / OneDrive):**
  - `[PASTE_PUBLIC_VIEWER_LINK_HERE]`

> Note: Verify that cloud link permissions are configured so that anyone with the link can view without login prompts.

---

## Slide Structure & Content Outline

1. **Title & Team:** Project title, Problem Statement ID, team members, and institutional affiliations.
2. **Context & Motivation:** Importance of mathematical optimization in logistics, operations research, and scheduling; motivations for building a modular, decoupled solver architecture.
3. **Pipeline Architecture:** Sequence diagram from input to solution:
   - MPS Ingestion → Model IR → Validation → Original Classification → Presolve (once) → Dispatch (`solveReduced`) → Solver Engine → Postsolve (once) → Feasibility Verification.
4. **Presolve & Postsolve Invertibility:**
   - How fixed variables, singleton rows, and redundant constraints are reduced while preserving index mappings.
   - Dual postsolve: Reconstructing shadow prices and reduced costs via bound-tightening provenance tracking, with stationarity validation and fail-closed error handling.
5. **Solver Engines:**
   - PDLP: First-order PDHG method with adaptive step sizing and Ruiz preconditioning for linear programs.
   - Dual Simplex: Tableau pivoting method providing basic solutions and serving as the relaxation solver for integer subproblems.
   - Branch-and-Cut: Branch-and-bound tree with Gomory fractional cuts and primal heuristics for MILP.
   - QP Engine: ADMM method with KKT factorizations for convex quadratic programs.
6. **CLI & Diagnostics:** Terminal interface, mascot banner, solve dashboard card, solver overrides, and output file export.
7. **Automated Testing & Correctness:** CTest suite execution covering presolve cascades, dual postsolve checks, engine conventions, and end-to-end pipelines.
8. **Roadmap & Future Extensions:**
   - Input format extension (`.lp` format reader).
   - Additional presolve reduction techniques (variable substitutions, binary probing).
   - Parallel tree search for branch-and-cut.
   - Barrier / interior point method for continuous models.
