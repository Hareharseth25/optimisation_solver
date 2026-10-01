import { test } from 'node:test';
import assert from 'node:assert/strict';
import { buildPipeline, renderPipeline, STAGES } from '../js/pipeline.js';
import { findAll, textOf } from '../js/vdom.js';
import { fixture } from './helpers.js';

const record = (name) => structuredClone(fixture(name).body.record);
const states = (pipeline) => Object.fromEntries(pipeline.stages.map((s) => [s.id, s.state]));
const stageOf = (pipeline, id) => pipeline.stages.find((s) => s.id === id);

const ALL_COMPLETED = Object.fromEntries(STAGES.map((id) => [id, 'completed']));

test('normal LP: every stage completed, ending after validation', () => {
  const r = record('lp_optimal');
  const p = buildPipeline(r);
  assert.deepEqual(p.stages.map((s) => s.id), STAGES);
  assert.deepEqual(states(p), ALL_COMPLETED);
  assert.equal(p.terminatedAt, 'validation');
  assert.equal(p.summary, 'Ran end to end: validation passed.');
  assert.deepEqual(stageOf(p, 'model').lines, ['3 variables · 1 constraint', 'minimize']);
  assert.equal(stageOf(p, 'classification').stateLabel, 'LP');
  assert.deepEqual(stageOf(p, 'presolve').lines, ['3 → 2 variables', '1 → 1 constraints']);
  assert.deepEqual(stageOf(p, 'dispatch').lines, ['selected: dual simplex']);
  assert.deepEqual(stageOf(p, 'engine').lines, ['executed: dual simplex', 'backend: CPU']);
  assert.deepEqual(stageOf(p, 'validation').lines, ['reduced space: passed', 'original space: passed']);
  assert.deepEqual([stageOf(p, 'result').stateLabel, ...stageOf(p, 'result').lines], ['optimal', 'objective 28']);
  // Timings are the record's own numbers, per stage.
  assert.equal(stageOf(p, 'presolve').seconds, r.stage_seconds.presolve);
  assert.equal(stageOf(p, 'engine').seconds, r.stage_seconds.engine);
  assert.equal(stageOf(p, 'postsolve').seconds, r.stage_seconds.postsolve);
  assert.equal(stageOf(p, 'validation').seconds, r.stage_seconds.reduced_validation);
  assert.equal(stageOf(p, 'result').seconds, r.stage_seconds.total);
});

test('successful postsolve: completed, with its own time', () => {
  const p = buildPipeline(record('lp_optimal'));
  const postsolve = stageOf(p, 'postsolve');
  assert.equal(postsolve.state, 'completed');
  assert.deepEqual(postsolve.lines, ['original coordinates']);
  assert.ok(postsolve.seconds > 0);
});

test('MILP: branch-and-cut executed, integral result', () => {
  const p = buildPipeline(record('milp_optimal'));
  assert.deepEqual(states(p), ALL_COMPLETED);
  assert.equal(stageOf(p, 'classification').stateLabel, 'MILP');
  assert.equal(stageOf(p, 'classification').lines[0], '3 binary');
  assert.equal(stageOf(p, 'engine').lines[0], 'executed: branch and cut');
  assert.equal(stageOf(p, 'result').lines[0], 'objective 7');
});

test('QP: routed to the QP engine', () => {
  const p = buildPipeline(record('qp_optimal'));
  assert.deepEqual(states(p), ALL_COMPLETED);
  assert.equal(stageOf(p, 'classification').stateLabel, 'QP');
  assert.equal(stageOf(p, 'engine').lines[0], 'executed: qp');
  assert.equal(stageOf(p, 'result').lines[0], 'objective -4.5');
});

test('presolve-infeasible: ends at presolve; nothing after it ran', () => {
  const p = buildPipeline(record('presolve_infeasible'));
  assert.deepEqual(states(p), {
    model: 'completed', classification: 'completed', presolve: 'infeasible', dispatch: 'not_run',
    engine: 'not_run', postsolve: 'not_run', validation: 'not_run', result: 'infeasible',
  });
  assert.equal(p.terminatedAt, 'presolve');
  assert.equal(p.summary, 'Ended at Presolve: proved infeasible.');
  assert.equal(stageOf(p, 'dispatch').stateLabel, 'not invoked');
  assert.deepEqual(stageOf(p, 'engine').lines, ['nothing executed']);
  for (const id of ['dispatch', 'engine', 'postsolve', 'validation']) {
    assert.equal(stageOf(p, id).seconds, null, `${id} has no invented time`);
  }
});

test('unsupported (no suitable engine): ends at dispatch; no engine stage ran', () => {
  const p = buildPipeline(record('unsupported'));
  assert.deepEqual(states(p), {
    model: 'completed', classification: 'completed', presolve: 'completed', dispatch: 'unsupported',
    engine: 'not_run', postsolve: 'not_run', validation: 'not_run', result: 'unsupported',
  });
  assert.equal(stageOf(p, 'dispatch').stateLabel, 'no suitable engine');
  assert.match(stageOf(p, 'dispatch').lines[0], /^non-convex quadratic objective/);
  assert.equal(p.terminatedAt, 'dispatch');
  assert.equal(p.summary, 'Ended at Dispatch: no suitable engine.');
});

test('selected but not executed (CUDA refused): dispatch chose, engine refused', () => {
  const r = record('cuda_refused');
  const p = buildPipeline(r);
  assert.deepEqual(states(p), {
    model: 'completed', classification: 'completed', presolve: 'completed', dispatch: 'completed',
    engine: 'refused', postsolve: 'not_run', validation: 'not_run', result: 'unsupported',
  });
  assert.deepEqual(stageOf(p, 'dispatch').lines, ['selected: pdlp']);
  const engine = stageOf(p, 'engine');
  assert.equal(engine.stateLabel, 'not executed');
  assert.deepEqual(engine.lines.slice(0, 2), ['selected: pdlp', 'executed: none']);
  assert.match(engine.lines[2], /^CUDA backend requested but unavailable/);
  // The engine path was entered (and timed) before the refusal; shown as such.
  assert.equal(engine.seconds, r.stage_seconds.engine);
  assert.equal(engine.timeLabel, 'engine path');
  assert.equal(p.terminatedAt, 'engine');
  assert.equal(p.summary, 'Ended at Engine: not executed.');
});

test('invalid model: KAIRO rejected it; no solver stage ran', () => {
  const p = buildPipeline(record('invalid_model'));
  assert.deepEqual(states(p), {
    model: 'failed', classification: 'not_run', presolve: 'not_run', dispatch: 'not_run',
    engine: 'not_run', postsolve: 'not_run', validation: 'not_run', result: 'failed',
  });
  assert.equal(stageOf(p, 'model').stateLabel, 'rejected by KAIRO');
  assert.equal(stageOf(p, 'result').stateLabel, 'invalid model');
  assert.equal(p.terminatedAt, 'model');
  assert.equal(p.summary, 'Ended at Model: rejected by KAIRO.');

  const unreadable = buildPipeline(record('unreadable_model'));
  assert.equal(stageOf(unreadable, 'model').stateLabel, 'could not be read');
  assert.match(stageOf(unreadable, 'model').lines[0], /unknown row sense/);
});

test('forced engine: dispatch honours the request, engine executes it', () => {
  const p = buildPipeline(record('forced_pdlp'));
  assert.deepEqual(states(p), ALL_COMPLETED);
  assert.deepEqual(stageOf(p, 'dispatch').lines, ['selected: pdlp']);
  assert.deepEqual(stageOf(p, 'engine').lines, ['executed: pdlp', 'backend: CPU']);
});

test('validation not run: the engine settled it (infeasible / unbounded)', () => {
  const infeasible = buildPipeline(record('engine_infeasible'));
  assert.deepEqual(states(infeasible), {
    model: 'completed', classification: 'completed', presolve: 'completed', dispatch: 'completed',
    engine: 'infeasible', postsolve: 'not_run', validation: 'not_run', result: 'infeasible',
  });
  assert.equal(stageOf(infeasible, 'engine').stateLabel, 'proved infeasible');
  assert.equal(infeasible.terminatedAt, 'engine');
  assert.equal(infeasible.summary, 'Ended at Engine: proved infeasible.');

  const unbounded = buildPipeline(record('unbounded'));
  assert.deepEqual(states(unbounded), {
    model: 'completed', classification: 'completed', presolve: 'completed', dispatch: 'completed',
    engine: 'unbounded', postsolve: 'not_run', validation: 'not_run', result: 'unbounded',
  });
  assert.equal(stageOf(unbounded, 'result').lines[0], 'no point returned');
});

test('limit reached: the rejected iterate fails reduced-space validation; postsolve never runs', () => {
  const p = buildPipeline(record('limit_reached'));
  assert.deepEqual(states(p), {
    model: 'completed', classification: 'completed', presolve: 'completed', dispatch: 'completed',
    engine: 'limit', postsolve: 'not_run', validation: 'failed', result: 'limit',
  });
  assert.deepEqual(stageOf(p, 'validation').lines, ['reduced space: constraint violation']);
  assert.equal(p.terminatedAt, 'validation');
  assert.equal(p.summary, 'Ended at Validation: failed.');
});

test('reconstruction failure and numerical failure map to failed stages', () => {
  const r = record('lp_optimal');
  r.termination.status = 'numerical_failure';
  r.validation.original_space = { ...r.validation.original_space, passed: false, status: 'invalid_mapping',
                                  failure: 'Invalid mapping' };
  const p = buildPipeline(r);
  assert.equal(stageOf(p, 'engine').state, 'failed');
  assert.equal(stageOf(p, 'postsolve').state, 'failed');
  assert.equal(stageOf(p, 'postsolve').stateLabel, 'reconstruction failed');
  assert.deepEqual(stageOf(p, 'validation').lines, ['reduced space: passed', 'original space: invalid mapping']);
  assert.equal(stageOf(p, 'result').stateLabel, 'solver failure');
});

test('states come from the record fields, not from the final status', () => {
  // Same optimal status, but the record says presolve and classification did
  // not run: the pipeline must say so rather than assume they did.
  const r = record('lp_optimal');
  r.classification = null;
  r.presolve = null;
  r.validation.reduced_space = null;
  const p = buildPipeline(r);
  assert.equal(stageOf(p, 'classification').state, 'not_run');
  assert.equal(stageOf(p, 'presolve').state, 'not_run');
  assert.deepEqual(stageOf(p, 'validation').lines, ['original space: passed']);
  assert.equal(stageOf(p, 'result').state, 'completed');
});

test('rendered rail: eight stages with their states, one termination point, links to sections', () => {
  const tree = renderPipeline(buildPipeline(record('presolve_infeasible')));
  const items = findAll(tree, (n) => n.tag === 'li');
  assert.deepEqual(items.map((n) => [n.attrs['data-stage-id'], n.attrs['data-stage-state']]), [
    ['model', 'completed'], ['classification', 'completed'], ['presolve', 'infeasible'],
    ['dispatch', 'not_run'], ['engine', 'not_run'], ['postsolve', 'not_run'],
    ['validation', 'not_run'], ['result', 'infeasible'],
  ]);
  const terminal = items.filter((n) => n.attrs.class.includes('stage-terminal'));
  assert.equal(terminal.length, 1);
  assert.match(textOf(terminal[0]), /Presolve proved infeasible .* ended here$/);
  const targets = findAll(tree, (n) => n.tag === 'button').map((n) => n.attrs['data-target']);
  assert.deepEqual(targets, ['model', 'model', 'presolve', 'dispatch', 'execution', 'validation', 'validation', 'run']);
  // Stages that did not run show no time at all.
  const timed = items.filter((n) => findAll(n, (c) => c.attrs.class === 'stage-time mono').length);
  assert.deepEqual(timed.map((n) => n.attrs['data-stage-id']), ['model', 'classification', 'presolve', 'result']);
  assert.match(textOf(items[3]), /Dispatch not invoked/);
  assert.match(textOf(items[4]), /Engine not run nothing executed/);
});

test('the pipeline appears above the detailed sections, which are all still present', async () => {
  const { renderResult } = await import('../js/report.js');
  const tree = renderResult(fixture('lp_optimal'), { fileName: 'm.mps', fileSize: 1 });
  const order = findAll(tree, (n) => n.attrs['data-section']).map((n) => n.attrs['data-section']);
  assert.deepEqual(order, ['pipeline', 'run', 'model', 'presolve', 'dispatch', 'execution', 'validation']);
});
