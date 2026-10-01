#!/usr/bin/env python3
"""Regenerates the UI test fixtures from REAL runs of the Explorer service.

Each fixture is {"httpStatus": ..., "body": ...} exactly as service.solve()
returned it for a repository model, so the UI is tested against records the
solver actually produced. Rerun after a record contract change:

    python3 explorer/web/tests/fixtures/make_fixtures.py build/optimsolver
"""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "explorer"))
from kairo_explorer import service  # noqa: E402

UNREADABLE = "NAME          BAD\nROWS\n N  OBJ\n Q  C1\nCOLUMNS\n    X  OBJ  1.0\nENDATA\n"
CASES = {
    "lp_optimal": ("tests/cli/presolve_reduction.mps", {}),
    "milp_optimal": ("tests/cli/knapsack_milp.mps", {}),
    "qp_optimal": ("tests/cli/convex_qp.mps", {}),
    "presolve_infeasible": ("tests/mps/test_cases/01_basic_lp.mps", {}),
    "unsupported": ("tests/cli/nonconvex_qp.mps", {}),
    "invalid_model": ("tests/cli/invalid_bounds.mps", {}),
    "unreadable_model": (None, {}),
    "forced_pdlp": ("tests/cli/simple_lp.mps", {"engine": "pdlp", "backend": "cpu"}),
    "limit_reached": ("tests/cli/simple_lp.mps", {"engine": "pdlp", "time_limit_seconds": 1e-30}),
    "rejected_option": ("tests/cli/simple_lp.mps", {"engine": "cplex"}),
}


def main(binary: str) -> None:
    out = Path(__file__).resolve().parent
    for name, (model, options) in CASES.items():
        content = UNREADABLE if model is None else (ROOT / model).read_text()
        request = json.dumps({"model": {"format": "mps", "content": content},
                              "options": {"threads": 1, **options}}).encode()
        status, body = service.solve(request, binary)
        (out / f"{name}.json").write_text(json.dumps({"httpStatus": status, "body": body}, indent=1) + "\n")
        print(f"{name}: HTTP {status} {body['outcome']}")


if __name__ == "__main__":
    main(sys.argv[1])
