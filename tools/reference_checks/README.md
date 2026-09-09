# External reference checks

These compare the solver against independent third-party implementations.
They are **not** part of `ctest`, because they need Python packages that the
build cannot assume are installed. The checks that need no external solver
live in `ctest` instead (`test_qp_convention`, `test_milp_enumeration`).

    pip install osqp scipy

## QP versus OSQP

Generates randomised convex QPs -- minimise and maximise, nonzero objective
offsets, non-diagonal Hessians with real cross terms, equality / ranged /
one-sided rows, and free variables -- solves each through the public
`solver::solve` API, and cross-checks against OSQP.

    g++ -std=c++17 -O2 -pthread \
        -I include -I pdlp_engine/include -I milp_engine/include -I qp_engine/include \
        tools/reference_checks/qp_random_generator.cpp \
        src/model/model.cpp src/util/parallel.cpp src/adapter/pdlp_adapter.cpp \
        src/solver/*.cpp src/presolve/*.cpp src/mps/*.cpp \
        pdlp_engine/src/*.cpp milp_engine/src/*.cpp qp_engine/src/*.cpp \
        -o /tmp/qp_gen
    /tmp/qp_gen 300 555555 > /tmp/qp_cases.txt
    python3 tools/reference_checks/qp_vs_osqp.py /tmp/qp_cases.txt

Arguments are `<count> <seed>`, so a run is reproducible. Every outcome is
accounted for: optimal verdicts are compared on objective AND solution, and
infeasible/unbounded verdicts are cross-checked against OSQP's own status
rather than skipped -- reporting only the solved cases would hide exactly the
weakness this is meant to find. The last line prints the full accounting.

Two things the harness deliberately does NOT do, both learned the hard way:

  * It appends an identity row per variable before handing the model to OSQP.
    OSQP has no separate variable-bound concept, only `l <= Ax <= u`, so
    omitting them silently solves an under-constrained problem and produces
    false "mismatches".
  * It does not check `P x + q + A^T y = 0` against the reported
    `constraintDuals`. Those cover the model's own rows only -- the QP
    adapter's appended bound rows are trimmed off -- so any active variable
    bound leaves a nonzero term by design. Solution comparison is used
    instead, which is valid because the generated Hessians are definite and
    the optimum is therefore unique.

## LP versus scipy/HiGHS

`lp_random_generator.cpp` emits randomised LPs including degenerate duplicate
rows, equalities, free variables and one-sided bounds, together with this
solver's verdict, for comparison against `scipy.optimize.linprog(method="highs")`.

Note when interpreting disagreements: HiGHS presolve conflates "infeasible or
unbounded" and has reported `infeasible` for instances that are provably
unbounded (a feasible point plus an improving recession ray both exist).
Confirm any status disagreement by hand before treating it as a defect here.
