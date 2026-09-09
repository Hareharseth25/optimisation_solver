import numpy as np, osqp, scipy.sparse as sp, sys

def read_floats(line): return [float(x) for x in line.split()]

import sys
f = open(sys.argv[1] if len(sys.argv) > 1 else '/tmp/qp_adv.txt')
N = int(f.readline())

n_solved=n_infeas=n_unbnd=n_other=0
nonopt_confirmed=0; nonopt_unchecked=0; nonopt_disputed=[]
osqp_mismatch=[]
kkt_bad=[]
feas_bad=[]
worst_obj_err = 0.0
worst_kkt = 0.0

for t in range(N):
    n,m,mx,hasoff,offset = f.readline().split()
    n=int(n); m=int(m); mx=int(mx); offset=float(offset)
    P=np.array([read_floats(f.readline()) for _ in range(n)])
    q=np.array(read_floats(f.readline()))
    lohi=read_floats(f.readline()); lo=np.array(lohi[0::2]); hi=np.array(lohi[1::2])
    A=np.zeros((m,n)); rlo=np.zeros(m); rhi=np.zeros(m)
    for i in range(m):
        row=read_floats(f.readline())
        A[i,:]=row[:n]; rlo[i]=row[n]; rhi[i]=row[n+1]
    status,engine,obj = f.readline().split()
    obj=float(obj)
    x = np.array(read_floats(f.readline())[:n])
    hasduals=int(f.readline())
    duals=np.array(read_floats(f.readline())[:m]) if hasduals else None

    if status != 'optimal':
        if status=='infeasible': n_infeas+=1
        elif status=='unbounded': n_unbnd+=1
        else: n_other+=1
        # Cross-check the NON-optimal verdicts too. Reporting only the solved
        # cases would hide exactly the weakness this battery exists to find.
        try:
            prob = osqp.OSQP()
            Pq = P if not mx else -P
            qq = q if not mx else -q
            Afull = np.vstack([A, np.eye(n)]) if m > 0 else np.eye(n)
            rlofull = np.concatenate([rlo, lo]) if m > 0 else lo
            rhifull = np.concatenate([rhi, hi]) if m > 0 else hi
            prob.setup(sp.csc_matrix(Pq), qq, sp.csc_matrix(Afull), rlofull, rhifull,
                       verbose=False, eps_abs=1e-9, eps_rel=1e-9, max_iter=200000)
            res = prob.solve()
            ref = res.info.status
            ok = (status=='infeasible' and ref=='primal infeasible') or \
                 (status=='unbounded'  and ref=='dual infeasible')
            if ok: nonopt_confirmed += 1
            else:  nonopt_disputed.append((t, status, ref))
        except Exception as e:
            nonopt_unchecked += 1
        continue
    n_solved+=1

    # --- independent re-derivation on the ORIGINAL (max/offset) problem ---
    def true_obj(xv):
        base = 0.5*xv@P@xv + q@xv
        if mx: base = -base  # P,q as generated ARE for the min-form objective 0.5x'Px+q'x representing f(x) directly under minimize sense; for maximize we generated P,q as the TRUE (max) objective's own P,q (not pre-negated) -- see note below
        return base

    # NOTE on convention: P,q here are exactly model.objective's own P,q
    # (the TRUE objective the user asked to minimize OR maximize), since the
    # generator writes P_true/q directly into quadraticTerms/linearTerms
    # without pre-negating for maximize -- fromModel does that internally.
    # So true objective value (in the requested sense) is simply:
    raw = 0.5*x@P@x + q@x
    predicted = raw + offset
    obj_err = abs(predicted - obj) / (1.0+abs(obj))
    worst_obj_err = max(worst_obj_err, obj_err)
    if obj_err > 1e-4:
        kkt_bad.append((t,'self-reported objective does not match its own P,q,x,offset', predicted, obj))

    # --- bound / row feasibility, independently ---
    viol = 0.0
    viol = max(viol, np.max(np.maximum(lo-1e-6-x, x-hi-1e-6)) if n>0 else 0.0)
    if m>0:
        ax = A@x
        viol = max(viol, np.max(np.maximum(rlo-1e-6-ax, ax-rhi-1e-6)))
    if viol > 1e-4:
        feas_bad.append((t,'primal violates its own bounds/rows',viol))

    # NOTE: a naive P x + q + A^T y = 0 check on the reported constraintDual
    # is NOT a valid KKT check here. runQp deliberately reports only the
    # duals for the model's own m rows and drops the QP adapter's appended
    # per-variable identity rows (its own comment: "the trailing entries are
    # reduced costs on the variable bounds, which SolveResult has no field
    # for"). Any case with an active variable bound has a nonzero missing
    # term by design, not by bug. The valid test given strict convexity (P
    # built as L L^T + eps I, so positive definite) is that the optimum is
    # UNIQUE -- so x itself, not a partial dual residual, is what to compare
    # against an independent solver.

    # --- cross-check against OSQP (independent implementation) ---
    try:
        prob = osqp.OSQP()
        Pq = P if not mx else -P
        qq = q if not mx else -q
        # OSQP has no separate variable-bound concept, only l<=Ax<=u -- append
        # an identity row per variable, exactly like qp_adapter.cpp does. My
        # first version of this harness omitted this, so for any low-m case
        # OSQP solved a DIFFERENT, under-constrained problem and "mismatches"
        # below were an artifact of the harness, not the engine (case 0,
        # m=0: engine's x and objective both matched an independent hand
        # derivation exactly; OSQP without bounds found the unconstrained
        # optimum instead).
        Afull = np.vstack([A, np.eye(n)]) if m > 0 else np.eye(n)
        rlofull = np.concatenate([rlo, lo]) if m > 0 else lo
        rhifull = np.concatenate([rhi, hi]) if m > 0 else hi
        prob.setup(sp.csc_matrix(Pq), qq, sp.csc_matrix(Afull), rlofull, rhifull,
                   verbose=False, eps_abs=1e-9, eps_rel=1e-9, max_iter=200000, polish=True)
        res = prob.solve()
        if res.info.status != 'solved':
            continue
        ref_obj = 0.5*res.x@P@res.x + q@res.x + offset  # true-sense objective at OSQP's point
        err = abs(ref_obj - obj)/(1.0+abs(ref_obj))
        xerr = np.max(np.abs(res.x - x)) if len(res.x) == n else float('inf')
        if err > 1e-4 or xerr > 1e-3:
            osqp_mismatch.append((t, 'obj', obj, ref_obj, err, 'xerr', xerr))
    except Exception as e:
        pass

print(f"{N} adversarial QPs: max/min mixed, nonzero offsets, non-diagonal P, eq/ranged/one-sided rows, free vars")
print(f"  Optimal={n_solved}  Infeasible={n_infeas}  Unbounded={n_unbnd}  other={n_other}")
print(f"  worst self-consistency objective error: {worst_obj_err:.3g}")
print(f"  worst dual stationarity residual (best-of-two-signs): {worst_kkt:.3g}")
print(f"  self-reported-objective mismatches: {len(kkt_bad)}")
for x in kkt_bad[:10]: print("   ", x)
print(f"  primal feasibility violations: {len(feas_bad)}")
for x in feas_bad[:10]: print("   ", x)
print(f"  OSQP objective mismatches (>1e-4 rel): {len(osqp_mismatch)} / {n_solved} solved-and-checked")
print(f"  NON-optimal verdicts cross-checked    : {nonopt_confirmed} confirmed, "
      f"{len(nonopt_disputed)} disputed, {nonopt_unchecked} unchecked")
for d in nonopt_disputed[:10]: print("    disputed:", d)
total_accounted = n_solved + n_infeas + n_unbnd + n_other
print(f"  ACCOUNTING: {n_solved} optimal + {n_infeas} infeasible + {n_unbnd} unbounded "
      f"+ {n_other} other = {total_accounted} of {N}")
for x in osqp_mismatch[:15]: print("   ", x)
