# SIH 2026 Submission Checklist

This checklist outlines the verification steps and deliverables required before final submission for SIH 2026.

---

## 1. Pre-Submission Checklist

Verify each item before submitting the repository link:

- [ ] **Working Source Code:** All solver, presolve, postsolve, and CLI code builds without errors or broken dependencies.
- [ ] **README.md Details:**
  - [ ] Official Problem Statement ID and title updated.
  - [ ] Team member names, roles, and institutional affiliations added.
  - [ ] Build and execution steps verified on a clean environment.
- [ ] **Architecture Documentation:** `docs/architecture.md` accurately documents the pipeline, design decisions, and solver engines.
- [ ] **Automated Testing:** All test suites execute cleanly via `ctest --test-dir build --output-on-failure`.
- [ ] **Terminal Screenshots:** Real screenshots of the CLI welcome screen, solve output, and test execution are saved to `assets/screenshots/`.
- [ ] **Presentation Slide Deck:** The final presentation file (`.pptx` or `.pdf`) is placed in `submission/` or linked in `submission/PRESENTATION.md`.
- [ ] **Demonstration Video:** The 3–5 minute video recording is uploaded to YouTube (unlisted/public) or Google Drive (public access) and linked in `submission/DEMO.md`.
- [ ] **Public Repository Access:** The GitHub repository is configured as **Public** so reviewers can access the code without authentication requests.
- [ ] **No Secrets or Build Artifacts:** Verify that no private credentials, temporary debug files, `.DS_Store` files, or `build/` directories are tracked in git.

---

## 2. Independent Verification Procedure

To verify the submission from a reviewer's perspective, run these commands in a clean directory:

```bash
# 1. Clone repository into a temporary directory
git clone https://github.com/RaghavGupta2910/optimisation_solver.git /tmp/verify_solver
cd /tmp/verify_solver

# 2. Configure and build
cmake -S . -B build
cmake --build build -j

# 3. Run the automated test suite
ctest --test-dir build --output-on-failure

# 4. Run CLI help and sample solve
./build/optimsolver --help
./build/optimsolver solve tests/cli/simple_lp.mps
./build/optimsolver solve tests/cli/presolve_reduction.mps --output /tmp/solution.txt

# 5. Confirm clean git status
git status
```

---

## 3. Deliverables Status Summary

| Deliverable | Location | Status |
| :--- | :--- | :--- |
| **Source Code & Build System** | `include/`, `src/`, `cli/`, `CMakeLists.txt` | Complete |
| **Architecture Documentation** | `docs/architecture.md` | Complete |
| **Test Suite** | `tests/` | Complete (all suites passing) |
| **Presentation Deck** | `submission/PRESENTATION.md` | Pending final slide deck file / link |
| **Demonstration Video** | `submission/DEMO.md` | Pending video recording / link |
| **Terminal Screenshots** | `assets/screenshots/` | Pending screenshot captures |
