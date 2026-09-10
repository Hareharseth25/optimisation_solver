# Screenshots

This directory is intended to store terminal captures and CLI screenshots for our documentation and SIH presentation.

---

## Planned Screenshots

The following terminal captures are planned before final submission:

| Filename | Command | Purpose |
| :--- | :--- | :--- |
| `01-welcome-screen.png` | `./build/optimsolver --help` | Header banner, mascot, and top-level commands |
| `02-solve-help.png` | `./build/optimsolver solve --help` | Solve options (`--solver`, `--time-limit`, `--output`) |
| `03-solve-dashboard.png` | `./build/optimsolver solve tests/cli/simple_lp.mps` | Solve dashboard card showing problem size, status, objective value, and solve time |
| `04-presolve-reduction.png` | `./build/optimsolver solve tests/cli/presolve_reduction.mps` | Dashboard showing dimension reduction (3 variables reduced to 2) and dual availability |
| `05-solver-override.png` | `./build/optimsolver solve tests/cli/simple_lp.mps --solver pdlp` | Solve dashboard showing PDLP engine selected via the `--solver` flag |
| `06-test-results.png` | `ctest --test-dir build --output-on-failure` | CTest output showing clean execution across all test suites |

---

## Guidelines

- Save files in standard PNG (`.png`) format.
- Use high-contrast, legible terminal captures.
- Use the numbered filenames above to preserve display ordering.
- Do not commit placeholder or mock image files; add only actual terminal captures.
