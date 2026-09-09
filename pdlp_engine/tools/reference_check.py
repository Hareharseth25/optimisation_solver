#!/usr/bin/env python3
"""Cross-checks PDLP engine answers against HiGHS.

Ranged rows (l <= Ax <= u) are passed to HiGHS natively through
scipy.optimize.milp, which wraps the same HiGHS build used by linprog.

Usage: reference_check.py <directory>
"""
import sys
import glob
import os
import time

import numpy as np
from scipy.optimize import milp, LinearConstraint, Bounds
from scipy.sparse import coo_matrix


def read_vec(handle, count):
    values = []
    while len(values) < count:
        values.extend(handle.readline().split())
    return np.array([float(v) for v in values[:count]])


def load(path):
    with open(path) as handle:
        rows, cols, nnz = (int(x) for x in handle.readline().split())
        offset = float(handle.readline())
        c = read_vec(handle, cols)
        xl = read_vec(handle, cols)
        xu = read_vec(handle, cols)
        rl = read_vec(handle, rows)
        ru = read_vec(handle, rows)
        data = np.loadtxt(handle, max_rows=nnz)
    if data.ndim == 1:
        data = data.reshape(1, -1)
    A = coo_matrix(
        (data[:, 2], (data[:, 0].astype(int), data[:, 1].astype(int))),
        shape=(rows, cols),
    ).tocsr()
    return A, c, offset, xl, xu, rl, ru


def main(directory):
    header = (f"{'instance':<12}{'HiGHS obj':>16}{'PDLP obj':>16}{'rel diff':>11}"
              f"{'HiGHS s':>9}{'PDLP s':>9}  {'PDLP status'}")
    print(header)
    print("-" * len(header))

    worst = 0.0
    for path in sorted(glob.glob(os.path.join(directory, "*.lp.txt"))):
        name = os.path.basename(path).split(".")[0]
        A, c, offset, xl, xu, rl, ru = load(path)

        start = time.time()
        res = milp(
            c=c,
            constraints=LinearConstraint(A, rl, ru),
            bounds=Bounds(xl, xu),
            integrality=np.zeros(len(c)),
        )
        highs_time = time.time() - start

        with open(os.path.join(directory, name + ".answer.txt")) as handle:
            parts = handle.readline().split()
        status, obj = parts[0], float(parts[1])
        pdlp_time = float(parts[5]) if len(parts) > 5 else float("nan")

        highs_label = {0: "optimal", 2: "infeasible", 3: "unbounded"}.get(
            res.status, f"status {res.status}")
        if res.status != 0:
            agree = ((highs_label == "infeasible" and status == "infeasible")
                     or (highs_label == "unbounded" and status == "unbounded"))
            mark = "agree" if agree else "MISMATCH"
            print(f"{name:<12}{highs_label:>16}{status:>16}{'':>11}"
                  f"{highs_time:>9.3f}{pdlp_time:>9.3f}  {mark}")
            continue

        if res.status == 0:
            ref = res.fun + offset
            rel = abs(obj - ref) / (1.0 + abs(ref))
            worst = max(worst, rel)
            print(f"{name:<12}{ref:>16.8g}{obj:>16.8g}{rel:>11.2e}"
                  f"{highs_time:>9.3f}{pdlp_time:>9.3f}  {status}")
        else:
            print(f"{name:<12}HiGHS status {res.status}: {res.message[:40]}")

    print(f"\nworst relative objective difference: {worst:.3e}")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else ".")
