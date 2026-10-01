import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readDispatch, isPseudoEngine, PSEUDO_ENGINES } from '../js/dispatch.js';
import { renderResult, summarize } from '../js/report.js';
import { buildPipeline } from '../js/pipeline.js';
import { textOf, findAll, byData } from '../js/vdom.js';
import { fixture } from './helpers.js';

const record = (name) => structuredClone(fixture(name).body.record);
const render = (name) => renderResult(fixture(name), { fileName: 'model.mps', fileSize: 1 });
const sectionText = (tree, name) => textOf(byData(tree, 'section', name) ?? '');
const field = (tree, name) => textOf(byData(tree, 'field', name) ?? '');
const role = (tree, name) => textOf(byData(tree, 'role', name) ?? '');
const stageLabel = (tree, key) => textOf(findAll(tree, (n) => n.attrs['data-stage'] === key)[0]?.children[0] ?? '');

// Every field the reader returns, for one real record.
function check(name, expected) {
  const dx = readDispatch(record(name));
  for (const [key, value] of Object.entries(expected)) assert.deepEqual(dx[key], value, `${name}.${key}`);
  return dx;
}

// ----------------------------------------------------- record interpretation

test('automatic LP / MILP / QP / MIQP: selected engine executed', () => {
  for (const [name, engine] of [['lp_optimal', 'dual_simplex'], ['milp_optimal', 'branch_and_cut'],
                                ['qp_optimal', 'qp'], ['miqp_optimal', 'miqp']]) {
    const dx = check(name, { state: 'invoked', mode: 'automatic', requested: null, selected: engine,
      pseudo: null, executed: engine, selectedNotExecuted: false, refusal: null, engineTimeKind: 'execution' });
    assert.equal(dx.reason, record(name).dispatch.reason, 'reason verbatim');
  }
});

test('forced engine: the caller\'s spelling, the dispatcher\'s canonical selection', () => {
  check('forced_pdlp', { mode: 'forced', requested: 'pdlp', selected: 'pdlp', executed: 'pdlp',
    reason: 'engine forced by the caller' });
  // "ipm" is an alias KAIRO accepts; the UI does not translate it.
  check('forced_alias', { mode: 'forced', requested: 'ipm', selected: 'barrier', executed: 'barrier' });
});

test('forced engine that cannot represent a valid model', () => {
  const dx = check('forced_incompatible', { state: 'invoked', mode: 'forced', requested: 'pdlp', selected: 'pdlp',
    pseudo: null, executed: null, selectedNotExecuted: true, backendRefusal: false, engineRejectedModel: true,
    engineTimeKind: 'path' });
  assert.match(dx.refusal.message, /3 integer variables/);
  assert.equal(record('forced_incompatible').termination.status, 'invalid_model', 'KAIRO status preserved');
  assert.equal(fixture('forced_incompatible').body.outcome, 'engine_rejected', 'service outcome');
});

test('CUDA refusal: selected, not executed, backend refusal', () => {
  const dx = check('cuda_refused', { selected: 'pdlp', executed: null, selectedNotExecuted: true,
    backendRefusal: true, engineRejectedModel: false, engineTimeKind: 'path' });
  assert.match(dx.refusal.message, /^CUDA backend requested but unavailable/);
  assert.equal(dx.refusal.backendReason, null, 'identical to the message, so not repeated');
  assert.deepEqual([dx.backend.requested, dx.backend.executed, dx.backend.mismatch], ['cuda', null, true]);
});

test('pseudo-engines are outcomes', () => {
  assert.deepEqual(Object.keys(PSEUDO_ENGINES), ['infeasible', 'trivial', 'unsupported']);
  assert.equal(isPseudoEngine('dual_simplex'), false);
  check('presolve_infeasible', { state: 'not_invoked', selected: 'infeasible', pseudo: 'infeasible', executed: null,
    selectedNotExecuted: false, refusal: null, engineTimeKind: null, dispatchSeconds: null, reducedModel: null });
  check('structured_milp', { state: 'invoked', selected: 'trivial', pseudo: 'trivial', executed: 'trivial',
    selectedNotExecuted: false, engineTimeKind: 'execution' });
  check('unsupported', { state: 'invoked', selected: 'unsupported', pseudo: 'unsupported', executed: null,
    selectedNotExecuted: false, refusal: null, engineTimeKind: null });
});

test('engine-proved infeasible / unbounded: the engine executed', () => {
  check('engine_infeasible', { selected: 'dual_simplex', executed: 'dual_simplex', engineTimeKind: 'execution' });
  check('unbounded', { selected: 'dual_simplex', executed: 'dual_simplex', engineTimeKind: 'execution' });
});

test('invalid / unreadable: dispatch.* is null, termination defaults are ignored', () => {
  for (const name of ['invalid_model', 'unreadable_model']) {
    const r = record(name);
    assert.equal(r.termination.dispatched_engine, 'unsupported', 'the default the record carries');
    check(name, { state: 'not_called', selected: null, pseudo: null, executed: null, reason: null,
      selectedNotExecuted: false, engineRejectedModel: false, engineTimeKind: null });
  }
});

test('reduced-model evidence is presolve.reduced_* of an invoked dispatcher', () => {
  const r = record('lp_optimal');
  assert.deepEqual(readDispatch(r).reducedModel,
    { variables: r.presolve.reduced_variables, constraints: r.presolve.reduced_constraints,
      nonzeros: r.presolve.reduced_nonzeros });
  assert.equal(readDispatch(record('presolve_infeasible')).reducedModel, null, 'not invoked: not given to it');
});

test('backend: auto resolving to cpu is not a mismatch; an unmet explicit request is', () => {
  assert.equal(readDispatch(record('lp_optimal')).backend.mismatch, false);
  assert.equal(readDispatch(record('forced_pdlp')).backend.mismatch, false);
  assert.equal(readDispatch(record('cuda_refused')).backend.mismatch, true);
});

// ------------------------------------------------------- rendered semantics

test('dispatch section follows request → dispatcher → why → selected → executed', () => {
  const tree = render('lp_optimal');
  const steps = findAll(byData(tree, 'section', 'dispatch'), (n) => n.attrs['data-step']).map((n) => n.attrs['data-step']);
  assert.deepEqual(steps, ['request', 'decision', 'why']);
  assert.equal(field(tree, 'request-mode'), 'Automatic — the dispatcher chooses');
  assert.match(sectionText(tree, 'dispatch'), /Why — KAIRO dispatcher small enough for the dual simplex \(1 rows, 2 nonzeros\)/);
  assert.equal(role(tree, 'selected-engine'), 'Selected engine dual simplex');
  assert.equal(role(tree, 'executed-engine'), 'Executed dual simplex');
  assert.equal(field(tree, 'reduced-evidence'),
    'Observed reduced model given to the dispatcher: 2 variables · 1 constraint · 2 nonzeros');
});

test('forced alias shows the caller\'s spelling and the selected engine separately', () => {
  const tree = render('forced_alias');
  assert.equal(field(tree, 'request-mode'), 'Forced by caller: ipm');
  assert.equal(role(tree, 'selected-engine'), 'Selected engine barrier');
});

test('forced incompatibility is not called an invalid model', () => {
  const { body, httpStatus } = fixture('forced_incompatible');
  const summary = summarize(body, httpStatus);
  assert.equal(summary.title, 'Requested engine cannot solve this model');
  assert.match(summary.detail, /^pdlp refused the model; the model itself was accepted and classified\. KAIRO: model has 3 integer variables/);
  assert.equal(summary.code, 'invalid_model · HTTP 422', 'the KAIRO status is still shown as evidence');
  const tree = render('forced_incompatible');
  assert.doesNotMatch(textOf(findAll(tree, (n) => n.attrs['data-view'] === 'summary')[0]), /^Invalid model/);
  assert.match(field(tree, 'refusal'), /^Engine rejected the model Why pdlp did not run, as reported by its execution path — not the dispatcher's reason\./);
  assert.match(field(tree, 'refusal-message'), /3 integer variables/);
  assert.equal(field(tree, 'reason'), 'engine forced by the caller', 'dispatcher reason kept separate');
  const pipeline = buildPipeline(record('forced_incompatible'));
  assert.equal(pipeline.stages.find((s) => s.id === 'model').state, 'completed');
  assert.equal(pipeline.stages.find((s) => s.id === 'result').stateLabel, 'engine rejected model');
});

test('CUDA refusal: dispatcher decision and backend refusal shown apart', () => {
  const tree = render('cuda_refused');
  assert.equal(summarize(fixture('cuda_refused').body, 422).title, 'Requested backend unavailable');
  assert.equal(field(tree, 'reason'), 'engine forced by the caller');
  assert.match(field(tree, 'refusal'), /^Backend refusal /);
  assert.match(field(tree, 'refusal-message'), /^CUDA backend requested but unavailable/);
  assert.equal(role(tree, 'selected-engine'), 'Selected engine pdlp');
  assert.equal(role(tree, 'executed-engine'), 'Executed Nothing executed');
  assert.equal(field(tree, 'execution-backend'), 'requested cuda (device 0) → executed none');
  assert.equal(field(tree, 'backend-mismatch'), 'The cuda request was not honoured.');
  assert.equal(field(tree, 'backend-reason'), '', 'the copied refusal text is not repeated in Execution');
  assert.match(field(tree, 'termination'), /^unsupported — CUDA backend requested but unavailable/);
  assert.equal(field(render('qp_optimal'), 'backend-reason'), 'auto -> cpu: 6 nonzeros is below cudaNonzeroThreshold');
});

test('engine-path time is never labelled as execution', () => {
  for (const name of ['cuda_refused', 'forced_incompatible']) {
    const tree = render(name);
    assert.equal(stageLabel(tree, 'engine'), 'Engine path (no engine executed)', name);
    assert.equal(field(tree, 'execution-engine'), 'nothing executed', name);
  }
  const executed = render('lp_optimal');
  assert.equal(stageLabel(executed, 'engine'), 'Engine execution');
  assert.equal(field(executed, 'execution-engine'), 'dual simplex');
});

test('presolve-infeasible: early exit, no engine, forced request not applied', () => {
  const tree = render('presolve_infeasible');
  assert.equal(field(tree, 'invoked'), 'Not invoked');
  assert.equal(role(tree, 'selected-engine'), 'Outcome before dispatch infeasible Presolve settled the solve; no engine is needed.');
  assert.equal(field(tree, 'refusal'), '');
  assert.equal(field(tree, 'reduced-evidence'), '');
  const r = record('presolve_infeasible');
  r.settings.requested_engine = 'pdlp';
  const forced = renderResult({ kind: 'response', httpStatus: 200, body: { ...fixture('presolve_infeasible').body, record: r } });
  assert.match(field(forced, 'request-mode'), /^Forced by caller: pdlp$/);
  assert.match(textOf(byData(forced, 'step', 'request')), /not applied, the dispatcher was not invoked/);
});

test('trivial is shown as a path, not an engine', () => {
  const tree = render('structured_milp');
  assert.equal(role(tree, 'selected-engine'),
    'Dispatch outcome trivial path The solution is read from variable bounds; no iterative engine runs.');
  assert.equal(role(tree, 'executed-engine'), 'Executed trivial path');
  assert.equal(field(tree, 'engine'), 'trivial path');
  assert.match(summarize(fixture('structured_milp').body, 200).detail, /^Solved on the trivial path/);
  assert.match(field(tree, 'reason'), /^no constraints remain/);
});

test('unsupported: no suitable engine, no refusal block', () => {
  const tree = render('unsupported');
  assert.equal(summarize(fixture('unsupported').body, 422).title, 'No suitable engine');
  assert.equal(field(tree, 'refusal'), '');
  assert.equal(field(tree, 'execution-engine'), 'nothing executed');
  assert.equal(stageLabel(tree, 'engine'), '', 'no engine time at all');
});

test('engine-proved infeasible / unbounded: executed, with termination shown', () => {
  const tree = render('engine_infeasible');
  assert.equal(role(tree, 'executed-engine'), 'Executed dual simplex');
  assert.equal(field(tree, 'termination'), 'infeasible — dual simplex');
  assert.equal(field(render('unbounded'), 'termination'), 'unbounded — dual simplex');
});

test('invalid / unreadable: dispatcher not reached, nothing from termination defaults', () => {
  for (const name of ['invalid_model', 'unreadable_model']) {
    const tree = render(name);
    assert.equal(field(tree, 'invoked'), 'Not reached', name);
    assert.doesNotMatch(sectionText(tree, 'dispatch'), /unsupported|Selected engine|Why/, name);
    assert.equal(field(tree, 'execution-engine'), 'nothing executed', name);
  }
});

test('termination message absent is said, not invented', () => {
  assert.equal(field(render('structured_milp'), 'termination'), 'optimal — no message');
});
