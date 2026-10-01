// Rendering of one solve, straight from the optimsolver.solve.v1 record.
//
// There is no frontend copy of the record: every section reads record fields
// directly and only formats them. Absent values (null) are rendered as
// "Not run" / "Unknown" / "None" with the reason, never as 0.

import { h } from './vdom.js';
import * as f from './format.js';
import { buildPipeline, renderPipeline } from './pipeline.js';

// ---------------------------------------------------------------------------
// Small building blocks
// ---------------------------------------------------------------------------

function notRun(reason) {
  return h('span', { class: 'not-run', 'data-state': 'not-run' },
    f.NOT_RUN, reason ? h('span', { class: 'not-run-reason' }, ` — ${reason}`) : null);
}

function badge(tone, text, extra = {}) {
  return h('span', { class: `badge badge-${tone}`, ...extra }, text);
}

function row(label, value, attrs = {}) {
  return h('div', { class: 'kv', ...attrs }, h('dt', {}, label), h('dd', {}, value));
}

function list(...rows) {
  return h('dl', { class: 'kv-list' }, rows);
}

function mono(text) {
  return h('span', { class: 'mono' }, text);
}

function section(name, title, ...content) {
  return h('section', { class: `panel panel-${name}`, 'data-section': name, 'aria-labelledby': `h-${name}` },
    h('h2', { id: `h-${name}`, class: 'panel-title', tabindex: '-1' }, title),
    content);
}

const engineName = (value) => (f.isAbsent(value) ? null : f.words(value));

// ---------------------------------------------------------------------------
// Headline: what happened, in the record's own terms
// ---------------------------------------------------------------------------

// [title, generic detail, what the user can do]
const RECORDLESS = {
  bad_request: ['Request not accepted', 'The Explorer service refused the request before running KAIRO.',
    'Check that the file is an MPS text file and the options are numbers where expected.'],
  rejected: ['KAIRO rejected an option', 'KAIRO refused an option value before reading the model.',
    'Adjust the solver options in the sidebar and run again.'],
  timeout: ['Timed out', 'KAIRO did not finish within the service\'s wall-clock guard.',
    'Set a time limit so KAIRO stops itself and reports its best result.'],
  solver_crashed: ['KAIRO stopped unexpectedly', 'The solver process terminated without a record.',
    'This is a KAIRO failure, not a problem with your options; please report it with the model.'],
  no_record: ['No result record', 'KAIRO finished without writing a record.', null],
  malformed_record: ['Unreadable result record', 'KAIRO wrote a record that could not be read.', null],
  unexpected_record: ['Unexpected result record', 'KAIRO wrote a record the Explorer does not understand.',
    'The Explorer and the optimsolver binary may be from different versions.'],
};

export function presolveProvedInfeasible(record) {
  return record?.termination?.status === 'infeasible' && record.presolve?.infeasible === true &&
         record.dispatch?.invoked === false;
}

// -> {tone: ok|warn|error, title, detail, code}
export function summarize(body, httpStatus) {
  const record = body.record;
  if (!record) {
    const [title, generic, hint] = RECORDLESS[body.outcome] ??
      ['Request failed', 'The Explorer service reported a failure.', null];
    return { tone: 'error', title, detail: body.error?.message || generic, hint,
             code: `${body.outcome}${httpStatus ? ` · HTTP ${httpStatus}` : ''}` };
  }
  const status = record.termination.status;
  const message = record.termination.message;
  const executed = engineName(record.termination.executed_engine);
  const code = `${status} · HTTP ${httpStatus ?? '?'}`;
  switch (status) {
    case 'optimal':
      return { tone: 'ok', title: 'Optimal', detail: `Solved by ${executed ?? 'KAIRO'}; the point passed validation.`, code };
    case 'limit_reached':
      return { tone: 'warn', title: 'Limit reached',
               detail: (record.primal ? 'Stopped at a limit; the best validated point found is shown.'
                                      : 'Stopped at a limit without a point that passed validation.') +
                       (message ? ` KAIRO: ${message}` : ''), code };
    case 'infeasible':
      return { tone: 'warn', title: 'Infeasible',
               detail: presolveProvedInfeasible(record)
                 ? 'Proved by presolve. The dispatcher was not invoked and no engine ran.'
                 : `Proved infeasible by ${executed ?? 'KAIRO'}.`, code };
    case 'unbounded':
      return { tone: 'warn', title: 'Unbounded', detail: message || `Proved unbounded by ${executed ?? 'KAIRO'}.`, code };
    case 'unsupported':
      return { tone: 'error', title: 'Unsupported', detail: message || 'No engine in KAIRO can solve this model as posed.', code };
    case 'invalid_model':
      return { tone: 'error', title: 'Invalid model', detail: message || 'KAIRO did not accept the model.', code };
    case 'numerical_failure':
      return { tone: 'error', title: 'Solver failure', detail: `KAIRO reported a numerical failure${message ? `: ${message}` : '.'}`, code };
    default:
      return { tone: 'error', title: f.words(status), detail: message, code };
  }
}

// ---------------------------------------------------------------------------
// Sections
// ---------------------------------------------------------------------------

function runSection(record, context) {
  const work = record.work ?? {};
  const backend = record.compute_backend?.executed;
  return section('run', 'Run',
    h('div', { class: 'run-figures' },
      h('div', { class: 'figure' }, h('div', { class: 'figure-label' }, 'Status'),
        h('div', { class: 'figure-value', 'data-field': 'status' }, f.words(record.termination.status))),
      h('div', { class: 'figure' }, h('div', { class: 'figure-label' }, 'Objective'),
        h('div', { class: 'figure-value mono', 'data-field': 'objective' },
          f.isAbsent(record.objective) ? 'None' : f.real(record.objective))),
      h('div', { class: 'figure' }, h('div', { class: 'figure-label' }, 'Engine executed'),
        h('div', { class: 'figure-value', 'data-field': 'engine' },
          engineName(record.termination.executed_engine) ?? 'None')),
      h('div', { class: 'figure' }, h('div', { class: 'figure-label' }, 'Backend'),
        h('div', { class: 'figure-value', 'data-field': 'backend' }, f.isAbsent(backend) ? 'None' : backend.toUpperCase())),
      h('div', { class: 'figure' }, h('div', { class: 'figure-label' }, 'Total solve time'),
        h('div', { class: 'figure-value mono', 'data-field': 'total' },
          f.isAbsent(record.stage_seconds?.total) ? notRun() : f.seconds(record.stage_seconds.total)))),
    list(
      row('Model file', context.fileName ? `${context.fileName}${context.fileSize !== undefined ? ` (${f.bytes(context.fileSize)})` : ''}` : f.UNKNOWN),
      row('Input SHA-256', mono(f.shortHash(record.instance?.sha256, 16))),
      f.isAbsent(work.iterations) ? null : row('Iterations', mono(f.count(work.iterations))),
      f.isAbsent(work.nodes) ? null : row('Nodes', mono(f.count(work.nodes))),
      row('KAIRO build', mono(`${f.shortHash(record.solver?.commit, 10)}${record.solver?.build_type ? ` · ${record.solver.build_type}` : ''}`)),
    ));
}

function modelSection(record) {
  const c = record.classification;
  const instance = record.instance ?? {};
  const sense = instance.objective_sense === 'max' ? 'Maximize' : instance.objective_sense === 'min' ? 'Minimize' : f.UNKNOWN;
  if (!c) {
    return section('model', 'Model',
      list(
        row('Classification', notRun(record.termination.status === 'invalid_model'
          ? 'KAIRO did not accept the model' : 'the solver was not called')),
        row('Variables', mono(f.count(instance.variables))),
        row('Constraints', mono(f.count(instance.constraints))),
        row('Objective', sense),
      ));
  }
  const flags = [
    c.has_network_structure && 'network matrix',
    c.has_big_m && `big-M rows (max ${f.real(c.max_big_m)})`,
    c.has_set_partitioning && 'set partitioning',
    c.symmetric_groups > 0 && `${f.count(c.symmetric_groups)} symmetric column group${c.symmetric_groups === 1 ? '' : 's'}`,
  ].filter(Boolean);
  return section('model', 'Model',
    h('div', { class: 'class-tag', 'data-field': 'problem-class' }, c.problem_class),
    list(
      row('Variables', mono(f.count(c.num_columns))),
      row('Constraints', mono(f.count(c.num_rows))),
      row('Nonzeros', mono(f.count(c.nonzeros))),
      row('Variable types', h('span', { class: 'types' },
        h('span', {}, mono(f.count(c.num_continuous)), ' continuous'),
        h('span', {}, mono(f.count(c.num_integer)), ' integer'),
        h('span', {}, mono(f.count(c.num_binary)), ' binary'))),
      row('Objective', sense),
      c.nonzeros > 0 ? row('Coefficient range', mono(`${f.real(c.coef_range_ratio)} : 1`)) : null,
      row('Structure', flags.length ? flags.join(' · ') : 'None detected'),
    ));
}

function change(original, reduced) {
  if (f.isAbsent(original) || f.isAbsent(reduced)) return '';
  const delta = reduced - original;
  return delta === 0 ? 'unchanged' : `${delta > 0 ? '+' : '−'}${f.count(Math.abs(delta))}`;
}

const TRANSFORMATION_LABELS = {
  fix_variable: ['fixed variable', 'fixed variables'],
  remove_variable: ['removed variable', 'removed variables'],
  remove_constraint: ['removed constraint', 'removed constraints'],
  substitute_variable: ['substitution', 'substitutions'],
  tighten_lower_bound: ['tightened lower bound', 'tightened lower bounds'],
  tighten_upper_bound: ['tightened upper bound', 'tightened upper bounds'],
};

function transformationLabel(kind, n) {
  const [one, many] = TRANSFORMATION_LABELS[kind] ?? [f.words(kind), f.words(kind)];
  return `${f.count(n)} ${n === 1 ? one : many}`;
}

function presolveSection(record) {
  const p = record.presolve;
  if (!p) {
    return section('presolve', 'Presolve', notRun(record.classification ? null : 'the model never reached presolve'));
  }
  const dims = [
    ['Variables', p.original_variables, p.reduced_variables],
    ['Constraints', p.original_constraints, p.reduced_constraints],
    ['Nonzeros', p.original_nonzeros, p.reduced_nonzeros],
  ];
  const kinds = Object.entries(p.transformations_by_type ?? {}).filter(([, n]) => n > 0);
  return section('presolve', 'Presolve',
    h('table', { class: 'reduction' },
      h('thead', {}, h('tr', {}, h('th', { scope: 'col' }, ''), h('th', { scope: 'col' }, 'Original'),
        h('th', { scope: 'col', 'aria-label': 'becomes' }, ''), h('th', { scope: 'col' }, 'Reduced'),
        h('th', { scope: 'col' }, 'Change'))),
      h('tbody', {}, dims.map(([label, original, reduced]) =>
        h('tr', { 'data-dimension': label.toLowerCase() },
          h('th', { scope: 'row' }, label), h('td', { class: 'mono' }, f.count(original)),
          h('td', { class: 'arrow', 'aria-hidden': 'true' }, '→'), h('td', { class: 'mono' }, f.count(reduced)),
          h('td', { class: 'mono delta' }, change(original, reduced)))))),
    list(
      row('State', p.infeasible ? badge('warn', 'Proved infeasible')
        : p.converged ? badge('ok', 'Converged') : badge('warn', 'Stopped at pass limit')),
      row('Transformations', h('span', {},
        mono(f.count(p.transformations)),
        kinds.length ? h('span', { class: 'muted' }, ` — ${kinds.map(([k, n]) => transformationLabel(k, n)).join(', ')}`) : null)),
    ));
}

function dispatchSection(record) {
  const d = record.dispatch;
  if (!d) return section('dispatch', 'Dispatch', notRun('the solver was not called'));
  const selected = engineName(d.engine);
  const executed = engineName(d.executed_engine);
  const requested = record.settings?.requested_engine;
  // The dispatcher can decide that NO engine applies; that is a decision, not a selection.
  const refused = d.invoked && d.engine === 'unsupported';
  const selectedLabel = !d.invoked ? 'Settled before dispatch' : refused ? 'Dispatcher decision' : 'Selected by dispatcher';
  return section('dispatch', 'Dispatch',
    h('div', { class: 'dispatch-state' },
      d.invoked ? badge('ok', 'Dispatcher invoked', { 'data-field': 'invoked' })
                : badge('neutral', 'Dispatcher not invoked', { 'data-field': 'invoked' }),
      h('span', { class: 'muted' }, requested ? ` Engine requested: ${f.words(requested)}` : ' Engine requested: automatic')),
    h('div', { class: 'engine-flow' },
      h('div', { class: 'engine-box engine-selected', 'data-role': 'selected-engine' },
        h('div', { class: 'engine-label' }, selectedLabel),
        h('div', { class: 'engine-name' }, selected ?? f.UNKNOWN)),
      h('div', { class: 'engine-arrow', 'aria-hidden': 'true' }, '→'),
      h('div', { class: `engine-box engine-executed${executed ? '' : ' engine-none'}`, 'data-role': 'executed-engine' },
        h('div', { class: 'engine-label' }, 'Executed'),
        h('div', { class: 'engine-name' }, executed ?? 'Nothing executed'))),
    d.reason ? h('blockquote', { class: 'reason', 'data-field': 'reason' }, d.reason) : null,
    refused ? h('p', { class: 'note' }, 'No KAIRO engine can solve this model as posed, so none ran.') : null,
    d.invoked && !refused && !executed
      ? h('p', { class: 'note' }, `The dispatcher chose ${selected}, but it did not run.`) : null,
    d.invoked && executed && executed !== selected
      ? h('p', { class: 'note' }, 'The executed engine differs from the selection.') : null);
}

const STAGES = [
  ['validation', 'Model validation'], ['classification', 'Classification'], ['presolve', 'Presolve'],
  ['dispatch', 'Dispatch'], ['engine', 'Engine'], ['reduced_validation', 'Reduced-space validation'],
  ['postsolve', 'Postsolve'],
];

function executionSection(record, process) {
  const backend = record.compute_backend ?? {};
  const stages = record.stage_seconds ?? {};
  const ran = STAGES.filter(([key]) => !f.isAbsent(stages[key]));
  const skipped = STAGES.filter(([key]) => f.isAbsent(stages[key]));
  return section('execution', 'Execution',
    list(
      row('Backend', h('span', {},
        `requested ${backend.requested ?? f.UNKNOWN} → executed `,
        f.isAbsent(backend.executed) ? h('strong', {}, 'none') : h('strong', {}, backend.executed),
        f.isAbsent(backend.executed_device) ? null : ` (device ${backend.executed_device})`)),
      backend.reason ? row('Backend note', h('span', { class: 'muted' }, backend.reason)) : null,
    ),
    ran.length === 0
      ? h('p', {}, notRun('no pipeline stage ran'))
      : h('table', { class: 'stages' },
        h('thead', {}, h('tr', {}, h('th', { scope: 'col' }, 'Stage'), h('th', { scope: 'col' }, 'Time'))),
        h('tbody', {},
          ran.map(([key, label]) => h('tr', { 'data-stage': key },
            h('th', { scope: 'row' }, label), h('td', { class: 'mono' }, f.seconds(stages[key])))),
          h('tr', { class: 'total', 'data-stage': 'total' }, h('th', { scope: 'row' }, 'Total'),
            h('td', { class: 'mono' }, f.isAbsent(stages.total) ? notRun() : f.seconds(stages.total))))),
    skipped.length && ran.length
      ? h('p', { class: 'note', 'data-field': 'skipped-stages' },
        `Did not run: ${skipped.map(([, label]) => label.toLowerCase()).join(', ')}.`) : null,
    h('p', { class: 'note muted' },
      `MPS read ${f.seconds(stages.parse)}`,
      process?.wall_seconds !== undefined && process?.wall_seconds !== null
        ? ` · request handled in ${f.seconds(process.wall_seconds)}` : ''));
}

function validationPass(title, check, name, extra) {
  if (!check) {
    return h('div', { class: 'check', 'data-check': name },
      h('h3', {}, title), notRun(name === 'reduced' ? 'no engine point to check' : 'postsolve did not run'));
  }
  return h('div', { class: 'check', 'data-check': name },
    h('h3', {}, title, ' ', check.passed ? badge('ok', 'Passed') : badge('error', 'Failed')),
    check.passed ? null : h('p', { class: 'failure' }, `${f.words(check.status)}${check.failure ? `: ${check.failure}` : ''}`),
    list(
      row('Max bound violation', mono(check.passed ? f.residual(check.max_bound_residual) : 'Not measured')),
      row('Max constraint violation', mono(check.passed ? f.residual(check.max_constraint_residual) : 'Not measured')),
      check.passed ? row('Scaled (bound · row)', mono(`${f.residual(check.max_bound_residual_scaled)} · ${f.residual(check.max_constraint_residual_scaled)}`)) : null,
      extra,
    ));
}

function validationSection(record) {
  const v = record.validation;
  if (!v) return section('validation', 'Validation', notRun('the solver was not called'));
  const reduced = v.reduced_space;
  const original = v.original_space;
  const c = record.classification;
  const integerModel = c && (c.num_binary + c.num_integer) > 0;
  const self = record.self_reported ?? {};

  const comparison = [
    reduced && !f.isAbsent(reduced.engine_reported_objective) ? ['Engine reported', reduced.engine_reported_objective] : null,
    reduced?.passed ? ['Recomputed, reduced model', reduced.objective] : null,
    original?.passed ? ['Recomputed, original model', original.objective] : null,
    !f.isAbsent(record.objective) ? ['Reported result', record.objective] : null,
  ].filter(Boolean);

  return section('validation', 'Validation',
    h('div', { class: 'checks' },
      validationPass('Reduced space', reduced, 'reduced'),
      validationPass('Original space', original, 'original',
        original ? row('Duals requested', original.duals_requested ? 'yes' : 'no') : null)),
    comparison.length
      ? h('div', { class: 'objective-compare', 'data-field': 'objective-comparison' },
        h('h3', {}, 'Objective'),
        list(...comparison.map(([label, value]) => row(label, mono(f.real(value))))))
      : null,
    list(
      row('Integrality', !integerModel ? h('span', { class: 'muted' }, 'Not applicable (continuous model)')
        : f.isAbsent(self.integrality_respected) ? h('span', { class: 'muted' }, 'Not applicable (no point)')
          : h('span', {}, self.integrality_respected ? badge('ok', 'Respected') : badge('error', 'Violated'),
            ' max violation ', mono(f.residual(self.max_integrality_violation)))),
      row('Duals', record.duals
        ? h('span', {}, 'available · max dual residual ', mono(f.residual(self.max_dual_residual)))
        : h('span', { class: 'muted' }, record.duals_unavailable_reason || 'Not available')),
    ));
}

// ---------------------------------------------------------------------------
// Whole result area
// ---------------------------------------------------------------------------

export function renderIdle() {
  return h('div', { class: 'empty', 'data-view': 'idle' },
    h('h2', {}, 'No run yet'),
    h('p', {}, 'Load an MPS model, choose solver options, and press Run. The full KAIRO record for the run appears here: model, presolve, dispatch, execution and validation.'));
}

export function renderRunning(fileName) {
  // Deliberately no percentages or stage names: KAIRO reports nothing until it finishes.
  return h('div', { class: 'running', 'data-view': 'running', role: 'status' },
    h('span', { class: 'spinner', 'aria-hidden': 'true' }),
    h('span', {}, 'Running KAIRO…'),
    fileName ? h('span', { class: 'muted' }, ` ${fileName}`) : null);
}

export function renderTransportError(message) {
  return h('div', { class: 'banner banner-error', 'data-view': 'error', role: 'alert' },
    h('div', { class: 'banner-title' }, 'Explorer service unavailable'),
    h('p', {}, message));
}

// result: postSolve() output; context: {fileName, fileSize}
export function renderResult(result, context = {}) {
  if (result.kind === 'transport') return renderTransportError(result.message);
  const { body, httpStatus } = result;
  const summary = summarize(body, httpStatus);
  const record = body.record;
  const banner = h('div', { class: `banner banner-${summary.tone}`, 'data-view': 'summary',
                            'data-tone': summary.tone, role: summary.tone === 'error' ? 'alert' : 'status' },
    h('div', { class: 'banner-head' },
      h('div', { class: 'banner-title' }, summary.title),
      h('code', { class: 'banner-code' }, summary.code)),
    summary.detail ? h(record ? 'p' : 'pre', { class: 'banner-detail' }, summary.detail) : null,
    summary.hint ? h('p', { class: 'banner-hint' }, summary.hint) : null);
  if (!record) return h('div', { class: 'result', 'data-view': 'result' }, banner);
  return h('div', { class: 'result', 'data-view': 'result' },
    banner,
    // "What happened?" first; the sections below are the evidence.
    renderPipeline(buildPipeline(record)),
    runSection(record, context),
    h('div', { class: 'grid' },
      modelSection(record),
      presolveSection(record),
      dispatchSection(record),
      executionSection(record, body.process),
      validationSection(record)));
}
