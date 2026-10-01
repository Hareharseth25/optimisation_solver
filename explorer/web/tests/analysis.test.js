import { test } from 'node:test';
import assert from 'node:assert/strict';
import { buildModelAnalysis, buildPresolveImpact, reductionPercent,
         renderModelAnalysis, renderPresolveImpact } from '../js/analysis.js';
import { findAll, textOf, byData } from '../js/vdom.js';
import { fixture } from './helpers.js';

const record = (name) => structuredClone(fixture(name).body.record);
const field = (tree, name) => textOf(byData(tree, 'field', name) ?? '');
const impactRow = (tree, key) => textOf(findAll(tree, (n) => n.attrs['data-impact'] === key)[0]);

// ------------------------------------------------------------ Model Analysis

test('LP: class, sense, dimensions and composition straight from classification', () => {
  const r = record('lp_optimal');
  const a = buildModelAnalysis(r);
  assert.equal(a.state, 'classified');
  assert.equal(a.problemClass, 'LP');
  assert.equal(a.sense, 'Minimize');
  assert.deepEqual(a.dimensions, { variables: r.classification.num_columns,
    constraints: r.classification.num_rows, nonzeros: r.classification.nonzeros });
  assert.deepEqual(a.dimensions, { variables: 3, constraints: 1, nonzeros: 3 });
  assert.deepEqual(a.composition, { continuous: 3, integer: 0, binary: 0 });
  const tree = renderModelAnalysis(a);
  assert.equal(field(tree, 'analysis-class'), 'LP');
  assert.equal(field(tree, 'analysis-sense'), 'Minimize');
  assert.equal(field(tree, 'analysis-composition'), 'Variable composition 3 continuous 0 integer 0 binary');
});

test('MILP (maximize): binary composition', () => {
  const a = buildModelAnalysis(record('milp_optimal'));
  assert.equal(a.problemClass, 'MILP');
  assert.equal(a.sense, 'Maximize');
  assert.deepEqual(a.composition, { continuous: 0, integer: 0, binary: 3 });
});

test('QP and MIQP classes come from the record', () => {
  assert.equal(buildModelAnalysis(record('qp_optimal')).problemClass, 'QP');
  const miqp = buildModelAnalysis(record('miqp_optimal'));
  assert.equal(miqp.problemClass, 'MIQP');
  // X is integer in [0, 3]: integer, not binary (the classifier's own count).
  assert.deepEqual(miqp.composition, { continuous: 1, integer: 1, binary: 0 });
});

test('structure flags: detected / not detected, with the recorded big-M and groups', () => {
  const structured = buildModelAnalysis(record('structured_milp'));
  assert.deepEqual(structured.structure.map((s) => [s.key, s.detected, s.detail]), [
    ['network', false, null], ['big_m', true, 'max 1000'], ['set_partitioning', true, null], ['symmetry', true, '1'],
  ]);
  const tree = renderModelAnalysis(structured);
  const structureText = field(tree, 'analysis-structure');
  assert.match(structureText, /Network structure Not detected/);
  assert.match(structureText, /Big-M rows Detected · max 1000/);
  assert.match(structureText, /Set partitioning Detected/);
  assert.match(structureText, /Symmetric column groups Detected · 1/);
  assert.doesNotMatch(textOf(tree), /score|quality/i);

  const network = buildModelAnalysis(record('network_flow'));
  assert.deepEqual(network.structure.find((s) => s.key === 'network'), { key: 'network', label: 'Network structure', detected: true, detail: null });
});

test('structure fields absent from a record are not shown', () => {
  const r = record('lp_optimal');
  delete r.classification.has_network_structure;
  delete r.classification.has_big_m;
  delete r.classification.max_big_m;
  delete r.classification.has_set_partitioning;
  delete r.classification.symmetric_groups;
  const a = buildModelAnalysis(r);
  assert.deepEqual(a.structure, []);
  assert.doesNotMatch(textOf(renderModelAnalysis(a)), /Network|Big-M|partitioning|Symmetric/);
});

test('zero nonzeros: no coefficient range', () => {
  const a = buildModelAnalysis(record('no_constraints'));
  assert.deepEqual(a.dimensions, { variables: 2, constraints: 0, nonzeros: 0 });
  assert.equal(a.coefRangeRatio, null);
  assert.doesNotMatch(textOf(renderModelAnalysis(a)), /Coefficient range/);
  assert.equal(buildModelAnalysis(record('lp_optimal')).coefRangeRatio, 1);
});

test('invalid model: classification not run, instance dimensions only, nothing inferred', () => {
  const a = buildModelAnalysis(record('invalid_model'));
  assert.equal(a.state, 'unclassified');
  assert.equal(a.problemClass, undefined);
  assert.deepEqual(a.dimensions, { variables: 1, constraints: 1, nonzeros: null });
  const text = textOf(renderModelAnalysis(a));
  assert.match(text, /Classification not run — KAIRO did not accept the model\./);
  assert.match(text, /Variables 1 Constraints 1 Objective Minimize/);
  assert.doesNotMatch(text, /\bLP\b|MILP|Nonzeros/);
});

test('unreadable model: unavailable, with the reader message and no dimensions', () => {
  const a = buildModelAnalysis(record('unreadable_model'));
  assert.equal(a.state, 'unreadable');
  assert.equal(a.sense, null);
  const text = textOf(renderModelAnalysis(a));
  assert.match(text, /Model not available — KAIRO could not read the file\./);
  assert.match(text, /unknown row sense/);
  assert.doesNotMatch(text, /Variables|\b0\b/);
});

// ----------------------------------------------------------- Presolve Impact

test('reduction percentage only for a positive original', () => {
  assert.equal(reductionPercent(3, 2), 100 / 3);
  assert.equal(reductionPercent(4, 4), 0);
  assert.equal(reductionPercent(6, 0), 100);
  assert.equal(reductionPercent(0, 0), null);
  assert.equal(reductionPercent(null, 1), null);
  assert.equal(reductionPercent(3, null), null);
});

test('normal reduction: presolve.* pairs, change and percentage', () => {
  const r = record('lp_optimal');
  const impact = buildPresolveImpact(r);
  assert.equal(impact.state, 'converged');
  assert.equal(impact.heading, 'Original → reduced');
  assert.deepEqual(impact.rows.map((x) => [x.key, x.original, x.reduced, x.change]), [
    ['variables', 3, 2, -1], ['constraints', 1, 1, 0], ['nonzeros', 3, 2, -1],
  ]);
  const tree = renderPresolveImpact(impact);
  assert.equal(impactRow(tree, 'variables'), 'Variables 3 2 −1 33.3 %');
  assert.equal(impactRow(tree, 'constraints'), 'Constraints 1 1 0 0 %');
  assert.equal(field(tree, 'impact-state'), 'Converged');
  assert.match(field(tree, 'impact-time'), /presolve time \d/);
});

test('the comparison uses presolve.* only, never instance or classification', () => {
  const r = record('lp_optimal');
  r.instance.variables = 999;
  r.classification.num_columns = 999;
  r.classification.nonzeros = 999;
  const impact = buildPresolveImpact(r);
  assert.deepEqual(impact.rows.map((x) => x.original), [3, 1, 3]);
});

test('large reduction to an empty model', () => {
  const impact = buildPresolveImpact(record('structured_milp'));
  assert.deepEqual(impact.rows.map((x) => [x.original, x.reduced, x.reduction]), [
    [4, 1, 75], [3, 0, 100], [6, 0, 100],
  ]);
});

test('unchanged model: 0 % reductions', () => {
  const impact = buildPresolveImpact(record('milp_optimal'));
  assert.deepEqual(impact.rows.map((x) => [x.change, x.reduction]), [[0, 0], [0, 0], [0, 0]]);
  assert.equal(impact.transformations, 0);
  assert.deepEqual(impact.byType, []);
  assert.match(field(renderPresolveImpact(impact), 'impact-transformations'), /^0 transformations logged$/);
});

test('zero denominators render as —, never NaN or Infinity', () => {
  const impact = buildPresolveImpact(record('no_constraints'));
  assert.deepEqual(impact.rows.map((x) => x.reduction), [0, null, null]);
  const tree = renderPresolveImpact(impact);
  assert.equal(impactRow(tree, 'constraints'), 'Constraints 0 0 0 —');
  assert.equal(impactRow(tree, 'nonzeros'), 'Nonzeros 0 0 0 —');
  assert.doesNotMatch(textOf(tree), /NaN|Infinity/);
});

test('presolve-infeasible: dimensions when stopped, and NO reduction percentages', () => {
  const impact = buildPresolveImpact(record('presolve_infeasible'));
  assert.equal(impact.state, 'infeasible');
  assert.equal(impact.stateLabel, 'Proved infeasible');
  assert.equal(impact.heading, 'Dimensions when presolve stopped');
  assert.deepEqual(impact.rows.map((x) => x.reduction), [null, null, null]);
  const tree = renderPresolveImpact(impact);
  assert.equal(findAll(tree, (n) => n.attrs['data-field'] === 'reduction').length, 0);
  assert.doesNotMatch(textOf(tree), /%|Reduction|Reduced\b/);
  assert.match(textOf(tree), /When stopped/);
  assert.equal(impactRow(tree, 'constraints'), 'Constraints 3 1 −2');
  assert.match(field(tree, 'impact-stopped-note'), /not a reduced model, so no reduction is reported/);
  // converged:true on this record is not presented as convergence.
  assert.doesNotMatch(textOf(tree), /Converged/);
});

test('non-converged (pass limit) is its own state', () => {
  const r = record('lp_optimal');
  r.presolve.converged = false;
  const impact = buildPresolveImpact(r);
  assert.equal(impact.state, 'not_converged');
  assert.equal(field(renderPresolveImpact(impact), 'impact-state'), 'Stopped before convergence (pass limit)');
  assert.equal(impact.rows[0].reduction, 100 / 3, 'a feasible stop still reports what changed');
});

test('transformation counts use the record vocabulary and are not called removals', () => {
  const tree = renderPresolveImpact(buildPresolveImpact(record('qp_optimal')));
  const text = field(tree, 'impact-transformations');
  assert.equal(text, '2 transformations logged tighten_upper_bound ×2');
  assert.doesNotMatch(textOf(tree), /removed|eliminated|deleted/i);

  const many = buildPresolveImpact(record('structured_milp'));
  assert.deepEqual(many.byType, [
    { type: 'remove_constraint', count: 3 }, { type: 'fix_variable', count: 3 },
    { type: 'tighten_lower_bound', count: 2 }, { type: 'tighten_upper_bound', count: 3 },
  ]);
  assert.equal(many.transformations, 11);
});

test('bound tightening only: dimensions unchanged, counts shown', () => {
  const impact = buildPresolveImpact(record('network_flow'));
  assert.deepEqual(impact.rows.map((x) => x.change), [0, 0, 0]);
  assert.deepEqual(impact.byType, [{ type: 'tighten_upper_bound', count: 3 }]);
});

test('not run: no table, no time', () => {
  for (const name of ['invalid_model', 'unreadable_model']) {
    const impact = buildPresolveImpact(record(name));
    assert.equal(impact.state, 'not_run');
    assert.equal(impact.seconds, null);
    const tree = renderPresolveImpact(impact);
    assert.equal(field(tree, 'impact-state'), 'Not run — the model never reached presolve');
    assert.equal(findAll(tree, (n) => n.tag === 'table').length, 0);
    assert.equal(byData(tree, 'field', 'impact-time'), undefined);
  }
});

test('no time is displayed when stage_seconds.presolve is null', () => {
  const r = record('lp_optimal');
  r.stage_seconds.presolve = null;
  const impact = buildPresolveImpact(r);
  assert.equal(impact.seconds, null);
  assert.equal(byData(renderPresolveImpact(impact), 'field', 'impact-time'), undefined);
});
