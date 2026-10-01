#!/usr/bin/env python3
"""SolveReport through `optimsolver solve --json`: the optimsolver.solve.v1
record keeps every existing field and type, and the sections that used to be
null (classification, presolve, presolve/postsolve stage times) are now filled
from the report of the same solve. Early exits must show which stages ran.

Timing checks assert sign and containment only, never wall-clock values.
"""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

binary = sys.argv[1]
root = Path(__file__).resolve().parents[2]
CLI = root / 'tests/cli'

# Every key of the v1 record as it existed before SolveReport, with the
# sub-keys each object had. New keys may be added; none may disappear.
V1_KEYS = {
    'schema': None,
    'instance': {'path', 'sha256', 'variables', 'constraints'},
    'solver': {'name', 'commit', 'build_type'},
    'compute_backend': {'requested', 'executed', 'requested_device', 'executed_device', 'reason'},
    'settings': {'requested_engine', 'time_limit_seconds', 'tolerance', 'thread_count'},
    'classification': {'problem_class', 'num_binary', 'num_integer', 'num_continuous',
                       'nonzeros', 'coef_range_ratio'},
    'presolve': {'infeasible', 'converged', 'reduced_variables', 'reduced_constraints',
                 'transformations'},
    'termination': {'status', 'message', 'dispatched_engine', 'executed_engine', 'engine_reason'},
    'objective': None, 'dual_bound': None, 'mip_gap': None, 'primal': None, 'duals': None,
    'reduced_costs': None, 'duals_unavailable_reason': None,
    'self_reported': {'max_bound_residual', 'max_constraint_residual', 'max_dual_residual',
                      'max_integrality_violation'},
    'stage_seconds': {'parse', 'presolve', 'solve', 'postsolve'},
    'work': {'iterations', 'nodes', 'solve_seconds'},
    'variable_names': None, 'constraint_names': None,
}
REPORT_STAGES = ('validation', 'classification', 'presolve', 'dispatch', 'engine',
                 'reduced_validation', 'postsolve')

failures = []


def check(ok, what):
    if not ok:
        failures.append(what)


def solve(tmp, model, *extra):
    out = Path(tmp) / 'record.json'
    out.unlink(missing_ok=True)
    process = subprocess.run([binary, 'solve', str(model), '--json', str(out), *extra],
                             capture_output=True, text=True, timeout=60)
    check(out.exists(), f'{model.name}: JSON written (exit {process.returncode})')
    return process, json.loads(out.read_text())


def check_v1_shape(label, record):
    check(record['schema'] == 'optimsolver.solve.v1', f'{label}: schema id unchanged')
    for key, children in V1_KEYS.items():
        check(key in record, f'{label}: v1 key {key} present')
        if children and isinstance(record.get(key), dict):
            missing = children - record[key].keys()
            check(not missing, f'{label}: {key} keeps {sorted(missing)}')
    if record.get('presolve') is not None:
        check(isinstance(record['presolve']['transformations'], int),
              f'{label}: presolve.transformations is still a count')


def check_stages(label, record, ran):
    stages = record['stage_seconds']
    for name in REPORT_STAGES:
        value = stages[name]
        if name in ran:
            check(isinstance(value, (int, float)) and value >= 0, f'{label}: {name} timed')
        else:
            check(value is None, f'{label}: {name} null because it did not run')
    total = stages['total']
    check(total == record['work']['solve_seconds'], f'{label}: total == work.solve_seconds')
    timed = [stages[name] for name in REPORT_STAGES if stages[name] is not None]
    check(sum(timed) <= total + 1e-9, f'{label}: stages fit inside total')
    check(stages['solve'] >= total, f'{label}: CLI solve time contains the pipeline total')


def check_dispatch_agrees(label, record):
    dispatch, term = record['dispatch'], record['termination']
    check(dispatch['engine'] == term['dispatched_engine'], f'{label}: dispatch.engine == termination')
    check(dispatch['executed_engine'] == term['executed_engine'],
          f'{label}: dispatch.executed_engine == termination')
    check((dispatch['reason'] or '') == term['engine_reason'], f'{label}: dispatch.reason == engine_reason')


def check_full_solve(label, record, problem_class, engine, objective, counts):
    check_v1_shape(label, record)
    cls = record['classification']
    check(cls is not None and cls['problem_class'] == problem_class, f'{label}: class {problem_class}')
    variables, constraints, nonzeros = counts
    check(cls['num_columns'] == variables == record['instance']['variables'], f'{label}: columns')
    check(cls['num_rows'] == constraints == record['instance']['constraints'], f'{label}: rows')
    check(cls['nonzeros'] == nonzeros, f'{label}: nonzeros {nonzeros}')
    check(cls['num_binary'] + cls['num_integer'] + cls['num_continuous'] == variables,
          f'{label}: type counts add up')
    pre = record['presolve']
    check(pre['original_variables'] == variables and pre['original_constraints'] == constraints,
          f'{label}: presolve original counts')
    check(pre['original_nonzeros'] == nonzeros, f'{label}: presolve original nonzeros')
    check(sum(pre['transformations_by_type'].values()) == pre['transformations'],
          f'{label}: per-type counts sum to the total')
    check(not pre['infeasible'], f'{label}: presolve feasible')
    check(record['dispatch']['invoked'] is True, f'{label}: dispatcher invoked')
    check(record['dispatch']['executed_engine'] == engine, f'{label}: executed {engine}')
    check_dispatch_agrees(label, record)
    val = record['validation']
    for space in ('reduced_space', 'original_space'):
        check(val[space] is not None and val[space]['passed'] and val[space]['status'] == 'success',
              f'{label}: {space} validation passed')
    original = val['original_space']
    check(abs(original['objective'] - record['objective']) <= 1e-9, f'{label}: validated objective')
    check(abs(record['objective'] - objective) <= 1e-6, f'{label}: objective {objective}')
    self_reported = record['self_reported']
    check(self_reported['max_bound_residual'] == original['max_bound_residual'] and
          self_reported['max_constraint_residual'] == original['max_constraint_residual'],
          f'{label}: self_reported residuals are postsolve\'s')
    check(record['termination']['status'] == 'optimal', f'{label}: optimal')
    check_stages(label, record, set(REPORT_STAGES))


with tempfile.TemporaryDirectory() as tmp:
    _, lp = solve(tmp, CLI / 'presolve_reduction.mps')
    check_full_solve('LP', lp, 'LP', 'dual_simplex', 28.0, (3, 1, 3))
    check(lp['presolve']['reduced_variables'] == 2 and lp['presolve']['reduced_nonzeros'] == 2,
          'LP: presolve removed the fixed column')
    check(lp['presolve']['transformations_by_type']['fix_variable'] == 1, 'LP: one fix_variable')
    check(lp['validation']['original_space']['duals_requested'] is True, 'LP: duals requested')
    check(lp['instance']['objective_sense'] == 'min', 'LP: objective sense')

    _, milp = solve(tmp, CLI / 'knapsack_milp.mps')
    check_full_solve('MILP', milp, 'MILP', 'branch_and_cut', 7.0, (3, 1, 3))
    check(milp['classification']['num_binary'] == 3, 'MILP: three binaries')
    check(milp['validation']['original_space']['duals_requested'] is False, 'MILP: no duals requested')
    check(milp['instance']['objective_sense'] == 'max', 'MILP: maximize')
    check(milp['self_reported']['integrality_respected'] is True, 'MILP: integral answer')

    _, qp = solve(tmp, CLI / 'convex_qp.mps')
    check_full_solve('QP', qp, 'QP', 'qp', -4.5, (2, 1, 2))

    _, forced = solve(tmp, CLI / 'simple_lp.mps', '--solver', 'pdlp', '--backend', 'cpu')
    check_v1_shape('forced', forced)
    check(forced['dispatch']['invoked'] and forced['dispatch']['engine'] == 'pdlp' and
          forced['dispatch']['reason'] == 'engine forced by the caller', 'forced: dispatch recorded')
    check(forced['settings']['requested_engine'] == 'pdlp', 'forced: request recorded')
    check(forced['compute_backend']['requested'] == 'cpu' and
          forced['compute_backend']['executed'] == 'cpu', 'forced: backend cpu ran')
    check_dispatch_agrees('forced', forced)

    # Presolve proved infeasibility: no dispatcher, engine, or validation.
    _, infeasible = solve(tmp, root / 'tests/mps/test_cases/01_basic_lp.mps')
    check_v1_shape('infeasible', infeasible)
    check(infeasible['termination']['status'] == 'infeasible', 'infeasible: status')
    check(infeasible['classification'] is not None, 'infeasible: classification ran')
    check(infeasible['presolve']['infeasible'] is True, 'infeasible: presolve flag')
    check(infeasible['dispatch']['invoked'] is False, 'infeasible: dispatcher not invoked')
    check(infeasible['dispatch']['executed_engine'] is None, 'infeasible: nothing executed')
    check_dispatch_agrees('infeasible', infeasible)
    check(infeasible['validation'] == {'reduced_space': None, 'original_space': None},
          'infeasible: no validation')
    check(infeasible['self_reported']['max_bound_residual'] is None, 'infeasible: no residuals')
    check(infeasible['self_reported']['integrality_respected'] is None, 'infeasible: no point to judge')
    check_stages('infeasible', infeasible, {'validation', 'classification', 'presolve'})

    # Unsupported: the dispatcher ran and refused; no engine.
    process, unsupported = solve(tmp, CLI / 'nonconvex_qp.mps')
    check(process.returncode == 1, 'unsupported: non-zero exit')
    check_v1_shape('unsupported', unsupported)
    check(unsupported['termination']['status'] == 'unsupported', 'unsupported: status')
    check(unsupported['dispatch']['invoked'] is True and
          unsupported['dispatch']['engine'] == 'unsupported' and
          unsupported['dispatch']['executed_engine'] is None, 'unsupported: dispatch refused')
    check('non-convex' in unsupported['dispatch']['reason'], 'unsupported: reason kept')
    check_dispatch_agrees('unsupported', unsupported)
    check_stages('unsupported', unsupported, {'validation', 'classification', 'presolve', 'dispatch'})

    # Time limit: PDLP hands back its unconverged iterate, reduced-space
    # validation rejects it, and postsolve never runs.
    _, limit = solve(tmp, CLI / 'simple_lp.mps', '--solver', 'pdlp', '--time-limit', '1e-30')
    check_v1_shape('limit', limit)
    check(limit['termination']['status'] == 'limit_reached', 'limit: status')
    check(limit['dispatch']['executed_engine'] == 'pdlp', 'limit: pdlp executed')
    check(limit['primal'] is None and limit['objective'] is None, 'limit: no point published')
    reduced = limit['validation']['reduced_space']
    check(reduced is not None and reduced['passed'] is False and
          reduced['status'] == 'constraint_violation' and reduced['failure'],
          'limit: the rejected iterate is reported as a failed check')
    check(reduced is not None and reduced['max_constraint_residual'] is None and
          reduced['objective'] is None, 'limit: no residuals claimed for a failed check')
    check(limit['validation']['original_space'] is None, 'limit: postsolve never ran')
    check(limit['self_reported']['max_bound_residual'] is None, 'limit: no self-reported residuals')
    check_stages('limit', limit, {'validation', 'classification', 'presolve', 'dispatch', 'engine',
                                  'reduced_validation'})

    # Refused by the CLI before the solver was called.
    process, invalid = solve(tmp, CLI / 'invalid_bounds.mps')
    check(process.returncode == 1, 'invalid: non-zero exit')
    check_v1_shape('invalid', invalid)
    check(invalid['termination']['status'] == 'invalid_model', 'invalid: status')
    for key in ('classification', 'presolve', 'dispatch', 'validation'):
        check(invalid[key] is None, f'invalid: {key} null')
    check(invalid['compute_backend']['executed'] is None, 'invalid: no backend ran')
    stages = invalid['stage_seconds']
    check(stages['parse'] is not None and stages['parse'] >= 0, 'invalid: parse timed')
    check(all(stages[k] is None for k in stages if k != 'parse'), 'invalid: nothing else ran')

    # Rejected by the MPS reader: still a record, with the reader's reason and
    # unknown (null) dimensions rather than zeros.
    unreadable = Path(tmp) / 'unreadable.mps'
    unreadable.write_text('NAME BAD\nROWS\n N  OBJ\n Q  C1\nCOLUMNS\n    X  OBJ  1.0\nENDATA\n')
    process, broken = solve(tmp, unreadable)
    check(process.returncode == 1, 'unreadable: non-zero exit')
    check_v1_shape('unreadable', broken)
    check(broken['termination']['status'] == 'invalid_model', 'unreadable: status')
    check('unknown row sense' in broken['termination']['message'], 'unreadable: reader reason kept')
    check(broken['instance']['variables'] is None and broken['instance']['constraints'] is None,
          'unreadable: dimensions unknown, not zero')
    check(broken['instance']['sha256'] is not None, 'unreadable: input bytes still identified')
    for key in ('classification', 'presolve', 'dispatch', 'validation', 'variable_names'):
        check(broken[key] is None, f'unreadable: {key} null')

for failure in failures:
    print('FAIL', failure)
if failures:
    sys.exit(1)
print('SolveReport JSON tests passed')
