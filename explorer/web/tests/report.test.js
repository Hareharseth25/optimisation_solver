import { test } from 'node:test';
import assert from 'node:assert/strict';
import { renderIdle, renderRunning, renderResult, summarize } from '../js/report.js';
import { textOf, findAll } from '../js/vdom.js';
import { render, section, text, field, role, notRuns } from './helpers.js';

test('LP: optimal run with every section populated from the record', () => {
  const { tree, record } = render('lp_optimal');
  assert.match(text(findAll(tree, (n) => n.attrs['data-view'] === 'summary')[0]), /^Optimal/);
  assert.equal(field(tree, 'status'), 'optimal');
  assert.equal(field(tree, 'objective'), '28');
  assert.equal(field(tree, 'engine'), 'dual simplex');
  assert.equal(field(tree, 'backend'), 'CPU');
  assert.equal(field(tree, 'problem-class'), 'LP');

  const presolve = section(tree, 'presolve');
  assert.match(text(findAll(presolve, (n) => n.attrs['data-dimension'] === 'variables')[0]), /Variables 3 → 2 −1/);
  assert.match(text(findAll(presolve, (n) => n.attrs['data-dimension'] === 'nonzeros')[0]), /Nonzeros 3 → 2/);
  assert.match(text(presolve), /Transformations 1 — 1 fixed variable$/);
  assert.match(text(presolve), /Converged/);

  // Every pipeline stage ran, so nothing says "Not run" anywhere.
  assert.equal(notRuns(tree).length, 0);
  const stages = findAll(section(tree, 'execution'), (n) => n.attrs['data-stage']).map((n) => n.attrs['data-stage']);
  assert.deepEqual(stages, ['validation', 'classification', 'presolve', 'dispatch', 'engine',
    'reduced_validation', 'postsolve', 'total']);

  const validation = section(tree, 'validation');
  assert.match(text(validation), /Reduced space Passed/);
  assert.match(text(validation), /Original space Passed/);
  assert.match(text(validation), /Recomputed, original model 28/);
  assert.match(text(validation), /Not applicable \(continuous model\)/);
  assert.equal(record.validation.original_space.max_bound_residual, 0);
});

test('selected and executed engines are rendered separately', () => {
  const { tree } = render('lp_optimal');
  assert.equal(role(tree, 'selected-engine'), 'Selected by dispatcher dual simplex');
  assert.equal(role(tree, 'executed-engine'), 'Executed dual simplex');
  assert.match(field(tree, 'reason'), /small enough for the dual simplex/);
  assert.equal(field(tree, 'invoked'), 'Dispatcher invoked');
});

test('MILP: branch-and-cut, binaries and integrality verdict', () => {
  const { tree } = render('milp_optimal');
  assert.equal(field(tree, 'problem-class'), 'MILP');
  assert.equal(field(tree, 'objective'), '7');
  assert.equal(role(tree, 'executed-engine'), 'Executed branch and cut');
  assert.match(text(section(tree, 'model')), /0 continuous 0 integer 3 binary/);
  assert.match(text(section(tree, 'model')), /Maximize/);
  assert.match(text(section(tree, 'validation')), /Integrality Respected max violation 0/);
  assert.match(text(section(tree, 'validation')), /Dual sensitivities are unavailable for integer models/);
  assert.match(text(section(tree, 'run')), /Nodes 1/);
});

test('QP: routed to the QP engine', () => {
  const { tree } = render('qp_optimal');
  assert.equal(field(tree, 'problem-class'), 'QP');
  assert.equal(field(tree, 'objective'), '-4.5');
  assert.equal(role(tree, 'executed-engine'), 'Executed qp');
});

test('presolve-proved infeasibility: no engine shown as executed, later stages not run', () => {
  const { tree, result } = render('presolve_infeasible');
  assert.equal(summarize(result.body, result.httpStatus).detail,
    'Proved by presolve. The dispatcher was not invoked and no engine ran.');
  assert.equal(field(tree, 'invoked'), 'Dispatcher not invoked');
  assert.equal(role(tree, 'selected-engine'), 'Settled before dispatch infeasible');
  assert.equal(role(tree, 'executed-engine'), 'Executed Nothing executed');
  assert.equal(field(tree, 'engine'), 'None');
  assert.equal(field(tree, 'objective'), 'None');
  assert.match(text(section(tree, 'presolve')), /Proved infeasible/);
  const stages = findAll(section(tree, 'execution'), (n) => n.attrs['data-stage']).map((n) => n.attrs['data-stage']);
  assert.deepEqual(stages, ['validation', 'classification', 'presolve', 'total']);
  assert.equal(field(tree, 'skipped-stages'),
    'Did not run: dispatch, engine, reduced-space validation, postsolve.');
  const validation = section(tree, 'validation');
  assert.equal(notRuns(validation).length, 2, 'both validation passes say Not run');
  assert.doesNotMatch(text(validation), /\b0\b/, 'no absent residual rendered as zero');
});

test('unsupported: dispatcher chose, nothing executed', () => {
  const { tree, result } = render('unsupported');
  assert.equal(result.httpStatus, 422);
  const summary = findAll(tree, (n) => n.attrs['data-view'] === 'summary')[0];
  assert.equal(summary.attrs.role, 'alert');
  assert.match(text(summary), /^Unsupported unsupported · HTTP 422 non-convex quadratic objective/);
  assert.equal(role(tree, 'selected-engine'), 'Dispatcher decision unsupported');
  assert.equal(role(tree, 'executed-engine'), 'Executed Nothing executed');
  assert.match(text(section(tree, 'dispatch')), /No KAIRO engine can solve this model as posed, so none ran\./);
  assert.doesNotMatch(text(section(tree, 'dispatch')), /chose/);
  assert.match(text(section(tree, 'execution')), /executed none/);
});

test('invalid model: every solver section says Not run, dimensions stay real', () => {
  const { tree } = render('invalid_model');
  assert.match(text(tree), /^Invalid model invalid_model · HTTP 422 model failed structural validation/);
  for (const name of ['presolve', 'dispatch', 'validation']) {
    assert.equal(notRuns(section(tree, name)).length, 1, `${name} not run`);
  }
  assert.match(text(section(tree, 'model')), /Classification Not run — KAIRO did not accept the model/);
  assert.match(text(section(tree, 'model')), /Variables 1/);
  assert.match(text(section(tree, 'execution')), /Not run — no pipeline stage ran/);
});

test('unreadable model: the reader message, and unknown (not zero) dimensions', () => {
  const { tree } = render('unreadable_model');
  assert.match(text(tree), /failed to read MPS file: .*unknown row sense/);
  assert.match(text(section(tree, 'model')), /Variables Unknown Constraints Unknown Objective Unknown/);
  assert.equal(field(tree, 'total'), 'Not run');
});

test('limit reached without a validated point', () => {
  const { tree } = render('limit_reached');
  assert.match(text(tree), /^Limit reached .*without a point that passed validation/);
  assert.equal(field(tree, 'objective'), 'None');
  const reduced = findAll(section(tree, 'validation'), (n) => n.attrs['data-check'] === 'reduced')[0];
  assert.match(textOf(reduced), /Reduced space Failed constraint violation: Constraint lower bound violation/);
  assert.match(textOf(reduced), /Max bound violation Not measured/);
});

test('forced engine shows the request and the forced reason', () => {
  const { tree } = render('forced_pdlp');
  assert.match(text(section(tree, 'dispatch')), /Engine requested: pdlp/);
  assert.equal(field(tree, 'reason'), 'engine forced by the caller');
  assert.match(text(section(tree, 'execution')), /requested cpu → executed cpu/);
});

test('rejected option: KAIRO message shown, no record sections', () => {
  const { tree } = render('rejected_option');
  assert.match(text(tree), /^KAIRO rejected an option rejected · HTTP 400 ✗ Invalid solver 'cplex'/);
  assert.match(text(tree), /Adjust the solver options in the sidebar and run again\.$/);
  assert.equal(section(tree, 'model'), undefined);
});

test('application failures without a record are explained, not swallowed', () => {
  const timeout = renderResult({ kind: 'response', httpStatus: 504,
    body: { outcome: 'timeout', record: null, error: { message: 'KAIRO did not finish within 630 s' }, process: null } });
  assert.match(textOf(timeout), /^Timed out timeout · HTTP 504 KAIRO did not finish within 630 s/);
  const transport = renderResult({ kind: 'transport', message: 'The Explorer service could not be reached. Is it running?' });
  assert.match(textOf(transport), /Explorer service unavailable/);
  assert.equal(transport.attrs.role, 'alert');
});

test('solver failure status maps to an error headline', () => {
  const { result } = render('lp_optimal');
  const body = structuredClone(result.body);
  body.outcome = 'solver_failure';
  body.record.termination.status = 'numerical_failure';
  body.record.termination.message = 'singular basis';
  const summary = summarize(body, 500);
  assert.equal(summary.tone, 'error');
  assert.equal(summary.title, 'Solver failure');
  assert.match(summary.detail, /numerical failure: singular basis/);
});

test('running state is an honest indeterminate status, with no fake progress', () => {
  const running = renderRunning('afiro.mps');
  assert.equal(running.attrs.role, 'status');
  assert.equal(textOf(running), 'Running KAIRO… afiro.mps');
  assert.doesNotMatch(textOf(running), /%|presolv|engine|stage/i);
  assert.match(textOf(renderIdle()), /No run yet/);
});

test('an engine chosen but refused before running is called out by name', () => {
  // The record shape KAIRO writes when an explicit CUDA request cannot be
  // honoured (test_solve_contract: chosen pdlp, executed nothing).
  const { result } = render('forced_pdlp');
  const body = structuredClone(result.body);
  body.outcome = 'unsupported';
  body.record.termination.status = 'unsupported';
  body.record.dispatch.executed_engine = null;
  body.record.termination.executed_engine = null;
  const tree = renderResult({ kind: 'response', httpStatus: 422, body });
  assert.equal(role(tree, 'selected-engine'), 'Selected by dispatcher pdlp');
  assert.equal(role(tree, 'executed-engine'), 'Executed Nothing executed');
  assert.match(text(section(tree, 'dispatch')), /The dispatcher chose pdlp, but it did not run\./);
});
