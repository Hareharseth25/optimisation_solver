// Model Analysis ("what did KAIRO receive?") and Presolve Impact ("what did
// presolve change?").
//
// Both builders are pure transformations of the optimsolver.solve.v1 record.
// They never look at the MPS, never classify, and never run or re-derive
// presolve: every number comes from a record field, and the only arithmetic
// is the reduction percentage between two presolve.* values.
//
//   Model Analysis   classification.{problem_class, num_columns, num_rows, nonzeros,
//                    num_continuous, num_integer, num_binary, has_network_structure,
//                    has_big_m, max_big_m, has_set_partitioning, symmetric_groups,
//                    coef_range_ratio}, instance.{objective_sense, variables, constraints}
//   Presolve Impact  presolve.{original_*, reduced_*, infeasible, converged,
//                    transformations, transformations_by_type}, stage_seconds.presolve

import { h } from './vdom.js';
import * as f from './format.js';

const present = (value) => value !== null && value !== undefined;

function senseOf(instance) {
  if (instance.objective_sense === 'max') return 'Maximize';
  if (instance.objective_sense === 'min') return 'Minimize';
  return null;
}

// ---------------------------------------------------------------------------
// Model Analysis
// ---------------------------------------------------------------------------

export function buildModelAnalysis(record) {
  const c = record.classification;
  const instance = record.instance ?? {};
  const sense = senseOf(instance);

  if (c) {
    // Only flags the record carries are shown; an older record without them
    // simply has no structure list.
    const structure = [];
    if (present(c.has_network_structure)) {
      structure.push({ key: 'network', label: 'Network structure', detected: c.has_network_structure, detail: null });
    }
    if (present(c.has_big_m)) {
      structure.push({ key: 'big_m', label: 'Big-M rows', detected: c.has_big_m,
                       detail: c.has_big_m && present(c.max_big_m) ? `max ${f.real(c.max_big_m)}` : null });
    }
    if (present(c.has_set_partitioning)) {
      structure.push({ key: 'set_partitioning', label: 'Set partitioning', detected: c.has_set_partitioning, detail: null });
    }
    if (present(c.symmetric_groups)) {
      structure.push({ key: 'symmetry', label: 'Symmetric column groups', detected: c.symmetric_groups > 0,
                       detail: c.symmetric_groups > 0 ? f.count(c.symmetric_groups) : null });
    }
    return {
      state: 'classified',
      problemClass: c.problem_class,
      sense,
      dimensions: { variables: c.num_columns, constraints: c.num_rows, nonzeros: c.nonzeros },
      composition: { continuous: c.num_continuous, integer: c.num_integer, binary: c.num_binary },
      structure,
      // The ratio is only meaningful when there are coefficients to compare.
      coefRangeRatio: c.nonzeros > 0 && present(c.coef_range_ratio) ? c.coef_range_ratio : null,
    };
  }

  // No classification: say why, and show only the dimensions the record has.
  if (!present(instance.variables)) {
    return { state: 'unreadable', sense: null, message: record.termination?.message ?? null,
             dimensions: { variables: null, constraints: null, nonzeros: null } };
  }
  return {
    state: 'unclassified',
    sense,
    reason: record.termination?.status === 'invalid_model'
      ? 'KAIRO did not accept the model' : 'classification did not run',
    dimensions: { variables: instance.variables, constraints: instance.constraints, nonzeros: null },
  };
}

// ---------------------------------------------------------------------------
// Presolve Impact
// ---------------------------------------------------------------------------

// 100 * (original - reduced) / original, only for a positive original.
export function reductionPercent(original, reduced) {
  if (!(typeof original === 'number' && original > 0) || typeof reduced !== 'number') return null;
  return (100 * (original - reduced)) / original;
}

const PRESOLVE_STATES = {
  infeasible: 'Proved infeasible',
  converged: 'Converged',
  not_converged: 'Stopped before convergence (pass limit)',
};

export function buildPresolveImpact(record) {
  const p = record.presolve;
  const seconds = present(record.stage_seconds?.presolve) ? record.stage_seconds.presolve : null;
  if (!p) {
    return { state: 'not_run', stateLabel: f.NOT_RUN, seconds,
             reason: record.classification ? null : 'the model never reached presolve' };
  }
  // An infeasible run stops presolve mid-way: its "reduced" numbers are the
  // dimensions at that moment, not a reduced model, and its converged flag
  // says nothing (it is only cleared for a feasible run at the pass limit).
  const state = p.infeasible ? 'infeasible' : p.converged ? 'converged' : 'not_converged';
  const stopped = state === 'infeasible';
  const rows = [
    ['variables', 'Variables', p.original_variables, p.reduced_variables],
    ['constraints', 'Constraints', p.original_constraints, p.reduced_constraints],
    ['nonzeros', 'Nonzeros', p.original_nonzeros, p.reduced_nonzeros],
  ].map(([key, label, original, reduced]) => ({
    key, label, original, reduced,
    change: typeof original === 'number' && typeof reduced === 'number' ? reduced - original : null,
    reduction: stopped ? null : reductionPercent(original, reduced),
  }));
  return {
    state,
    stateLabel: PRESOLVE_STATES[state],
    stopped,
    heading: stopped ? 'Dimensions when presolve stopped' : 'Original → reduced',
    rows,
    transformations: p.transformations,
    // Record vocabulary, unchanged: counts of logged transformations, not of
    // entities removed (a bound tightening removes nothing).
    byType: Object.entries(p.transformations_by_type ?? {})
      .filter(([, count]) => count > 0)
      .map(([type, count]) => ({ type, count })),
    seconds,
  };
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

function formatPercent(value) {
  if (value === null) return '—';
  if (value === 0) return '0 %';
  return `${Number(value.toFixed(1))} %`;
}

function formatChange(value) {
  if (value === null) return '—';
  if (value === 0) return '0';
  return `${value > 0 ? '+' : '−'}${f.count(Math.abs(value))}`;
}

function figure(label, value, field) {
  return h('div', { class: 'figure' }, h('div', { class: 'figure-label' }, label),
    h('div', { class: 'figure-value mono', 'data-field': field }, value));
}

export function renderModelAnalysis(analysis) {
  const d = analysis.dimensions;
  const body = [];
  if (analysis.state === 'classified') {
    body.push(
      h('div', { class: 'analysis-head' },
        h('span', { class: 'class-tag', 'data-field': 'analysis-class' }, analysis.problemClass),
        analysis.sense ? h('span', { class: 'analysis-sense', 'data-field': 'analysis-sense' }, analysis.sense) : null),
      h('div', { class: 'run-figures analysis-figures' },
        figure('Variables', f.count(d.variables), 'analysis-variables'),
        figure('Constraints', f.count(d.constraints), 'analysis-constraints'),
        figure('Nonzeros', f.count(d.nonzeros), 'analysis-nonzeros')),
      h('div', { class: 'composition', 'data-field': 'analysis-composition' },
        h('span', { class: 'composition-label' }, 'Variable composition'),
        ['continuous', 'integer', 'binary'].map((kind) =>
          h('span', { class: 'composition-item', 'data-kind': kind },
            h('span', { class: 'mono' }, f.count(analysis.composition[kind])), ` ${kind}`))));
    if (analysis.structure.length || analysis.coefRangeRatio !== null) {
      body.push(h('dl', { class: 'kv-list structure', 'data-field': 'analysis-structure' },
        analysis.structure.map((s) => h('div', { class: 'kv', 'data-structure': s.key },
          h('dt', {}, s.label),
          h('dd', {}, s.detected ? 'Detected' : 'Not detected',
            s.detail ? h('span', { class: 'muted mono' }, ` · ${s.detail}`) : null))),
        analysis.coefRangeRatio !== null
          ? h('div', { class: 'kv', 'data-structure': 'coef_range' },
            h('dt', {}, 'Coefficient range'), h('dd', { class: 'mono' }, `${f.real(analysis.coefRangeRatio)} : 1`))
          : null));
    }
  } else if (analysis.state === 'unreadable') {
    body.push(h('p', { class: 'not-run', 'data-state': 'not-run' },
      'Model not available — KAIRO could not read the file.'),
    analysis.message ? h('p', { class: 'note' }, analysis.message) : null);
  } else {
    body.push(
      h('p', {}, h('span', { class: 'not-run', 'data-state': 'not-run' }, `Classification not run — ${analysis.reason}.`)),
      h('dl', { class: 'kv-list' },
        h('div', { class: 'kv' }, h('dt', {}, 'Variables'), h('dd', { class: 'mono', 'data-field': 'analysis-variables' }, f.count(d.variables))),
        h('div', { class: 'kv' }, h('dt', {}, 'Constraints'), h('dd', { class: 'mono', 'data-field': 'analysis-constraints' }, f.count(d.constraints))),
        analysis.sense ? h('div', { class: 'kv' }, h('dt', {}, 'Objective'), h('dd', { 'data-field': 'analysis-sense' }, analysis.sense)) : null));
  }
  return h('section', { class: 'panel panel-model-analysis', 'data-section': 'model-analysis', 'aria-labelledby': 'h-model-analysis' },
    h('h2', { id: 'h-model-analysis', class: 'panel-title', tabindex: '-1' }, 'Model analysis'),
    h('p', { class: 'panel-question' }, 'What did KAIRO receive?'),
    body);
}

const STATE_TONES = { converged: 'ok', infeasible: 'warn', not_converged: 'warn' };

export function renderPresolveImpact(impact) {
  const title = [
    h('h2', { id: 'h-presolve-impact', class: 'panel-title', tabindex: '-1' }, 'Presolve impact'),
    h('p', { class: 'panel-question' }, 'What did presolve change?'),
  ];
  const wrap = (...content) => h('section', { class: 'panel panel-presolve-impact', 'data-section': 'presolve-impact',
                                               'aria-labelledby': 'h-presolve-impact' }, title, content);
  if (impact.state === 'not_run') {
    return wrap(h('p', { 'data-field': 'impact-state' },
      h('span', { class: 'not-run', 'data-state': 'not-run' }, f.NOT_RUN, impact.reason ? ` — ${impact.reason}` : '')));
  }
  return wrap(
    h('div', { class: 'impact-state' },
      h('span', { class: `badge badge-${STATE_TONES[impact.state]}`, 'data-field': 'impact-state' }, impact.stateLabel),
      impact.seconds !== null
        ? h('span', { class: 'muted', 'data-field': 'impact-time' }, ' presolve time ', h('span', { class: 'mono' }, f.seconds(impact.seconds)))
        : null),
    h('h3', { class: 'impact-heading', 'data-field': 'impact-heading' }, impact.heading),
    impact.stopped
      ? h('p', { class: 'note', 'data-field': 'impact-stopped-note' },
        'Presolve proved the model infeasible and stopped; these are not a reduced model, so no reduction is reported.')
      : null,
    h('table', { class: 'impact' },
      h('thead', {}, h('tr', {},
        h('th', { scope: 'col' }, ''), h('th', { scope: 'col' }, 'Original'),
        h('th', { scope: 'col' }, impact.stopped ? 'When stopped' : 'Reduced'),
        h('th', { scope: 'col' }, 'Change'),
        impact.stopped ? null : h('th', { scope: 'col' }, 'Reduction'))),
      h('tbody', {}, impact.rows.map((r) => h('tr', { 'data-impact': r.key },
        h('th', { scope: 'row' }, r.label),
        h('td', { class: 'mono' }, f.count(r.original)),
        h('td', { class: 'mono' }, f.count(r.reduced)),
        h('td', { class: 'mono delta' }, formatChange(r.change)),
        impact.stopped ? null : h('td', { class: 'mono', 'data-field': 'reduction' }, formatPercent(r.reduction)))))),
    h('div', { class: 'impact-transformations', 'data-field': 'impact-transformations' },
      h('span', {}, h('span', { class: 'mono' }, f.count(impact.transformations)),
        impact.transformations === 1 ? ' transformation logged' : ' transformations logged'),
      impact.byType.length
        ? h('ul', { class: 'transformation-types' }, impact.byType.map((t) =>
          h('li', { 'data-transformation': t.type }, h('code', {}, t.type), h('span', { class: 'mono' }, ` ×${f.count(t.count)}`))))
        : null));
}
