// The KAIRO solver pipeline view: "what happened?" for one run.
//
// buildPipeline(record) is a pure mapping from the optimsolver.solve.v1 record
// to presentation state. Every stage state is read from a record field that
// says whether that stage ran and what it concluded; nothing is inferred from
// the final status when a field already answers the question, and nothing is
// re-derived about the model or the engines.
//
//   stage            ran when                          verdict from
//   model            always (a record exists)          classification / termination (invalid_model)
//   classification   classification != null            classification.problem_class
//   presolve         presolve != null                  presolve.infeasible / converged
//   dispatch         dispatch.invoked                  dispatch.engine ("unsupported" = no engine)
//   engine           dispatch.executed_engine != null  termination.status
//                    (selected but not executed: dispatch.invoked, an engine chosen, executed null)
//   postsolve        validation.original_space != null original_space.status (reconstruction)
//   validation       either validation space != null   *.passed / *.status
//   result           always                            termination.status, objective
//
// Note on order: KAIRO checks the engine's point in reduced space BEFORE
// postsolve and checks the reconstructed point in original space as part of
// postsolve. The Validation stage reports both checks, labelled by space.

import { h } from './vdom.js';
import * as f from './format.js';
import { readDispatch, PSEUDO_ENGINES } from './dispatch.js';

export const STAGES = ['model', 'classification', 'presolve', 'dispatch', 'engine', 'postsolve', 'validation', 'result'];

// state -> visual tone
const TONES = {
  completed: 'ok', not_run: 'muted', failed: 'error', unsupported: 'error', refused: 'error',
  infeasible: 'warn', unbounded: 'warn', limit: 'warn',
};

// termination.status -> result-stage state and wording
const RESULT_STATES = {
  optimal: ['completed', 'optimal'],
  infeasible: ['infeasible', 'infeasible'],
  unbounded: ['unbounded', 'unbounded'],
  limit_reached: ['limit', 'limit reached'],
  unsupported: ['unsupported', 'unsupported'],
  invalid_model: ['failed', 'invalid model'],
  numerical_failure: ['failed', 'solver failure'],
};

// termination.status of a run whose engine executed -> engine-stage state
const ENGINE_STATES = {
  optimal: ['completed', 'executed'],
  infeasible: ['infeasible', 'proved infeasible'],
  unbounded: ['unbounded', 'proved unbounded'],
  limit_reached: ['limit', 'stopped at a limit'],
  numerical_failure: ['failed', 'numerical failure'],
  invalid_model: ['failed', 'refused the model'],
};

const name = (engine) => PSEUDO_ENGINES[engine]?.label ?? f.words(engine);
const plural = (n, one, many) => `${f.count(n)} ${n === 1 ? one : many}`;

function stage(id, label, target, state, stateLabel, lines = [], seconds = null, timeLabel = null) {
  return { id, label, target, state, tone: TONES[state], stateLabel,
           lines: lines.filter(Boolean), seconds: f.isAbsent(seconds) ? null : seconds, timeLabel, terminal: false };
}

const notRun = (id, label, target, stateLabel = 'not run', lines = []) =>
  stage(id, label, target, 'not_run', stateLabel, lines);

export function buildPipeline(record) {
  const term = record.termination;
  const st = record.stage_seconds ?? {};
  const instance = record.instance ?? {};
  const c = record.classification;
  const p = record.presolve;
  const d = record.dispatch;
  const v = record.validation;
  const stages = [];

  // MODEL -- KAIRO read and structurally checked the input. A model it
  // refused has no classification and status invalid_model.
  const refusedModel = term.status === 'invalid_model' && !c;
  stages.push(refusedModel
    ? stage('model', 'Model', 'model', 'failed',
        f.isAbsent(instance.variables) ? 'could not be read' : 'rejected by KAIRO',
        [term.message], st.parse, 'read')
    : stage('model', 'Model', 'model', 'completed', 'accepted',
        [`${plural(instance.variables, 'variable', 'variables')} · ${plural(instance.constraints, 'constraint', 'constraints')}`,
         instance.objective_sense === 'max' ? 'maximize' : instance.objective_sense === 'min' ? 'minimize' : null],
        st.parse, 'read'));

  // CLASSIFICATION
  stages.push(c
    ? stage('classification', 'Classification', 'model', 'completed', c.problem_class,
        [[c.num_continuous && `${f.count(c.num_continuous)} continuous`,
          c.num_integer && `${f.count(c.num_integer)} integer`,
          c.num_binary && `${f.count(c.num_binary)} binary`].filter(Boolean).join(' · ') || null,
         `${f.count(c.nonzeros)} nonzeros`],
        st.classification)
    : notRun('classification', 'Classification', 'model'));

  // PRESOLVE
  stages.push(p
    ? stage('presolve', 'Presolve', 'presolve', p.infeasible ? 'infeasible' : 'completed',
        p.infeasible ? 'proved infeasible' : p.converged ? 'completed' : 'stopped at pass limit',
        [`${f.count(p.original_variables)} → ${f.count(p.reduced_variables)} variables`,
         `${f.count(p.original_constraints)} → ${f.count(p.reduced_constraints)} constraints`],
        st.presolve)
    : notRun('presolve', 'Presolve', 'presolve'));

  // DISPATCH -- "unsupported" from an invoked dispatcher means no engine applies.
  const noEngine = d?.invoked === true && d.engine === 'unsupported';
  if (!d) stages.push(notRun('dispatch', 'Dispatch', 'dispatch'));
  else if (!d.invoked) stages.push(notRun('dispatch', 'Dispatch', 'dispatch', 'not invoked'));
  else if (noEngine) stages.push(stage('dispatch', 'Dispatch', 'dispatch', 'unsupported', 'no suitable engine', [d.reason], st.dispatch));
  else stages.push(stage('dispatch', 'Dispatch', 'dispatch', 'completed', 'invoked', [`selected: ${name(d.engine)}`], st.dispatch));

  // ENGINE -- executed is what ran, recorded by the code path that ran it.
  const executed = d?.executed_engine ?? null;
  const backend = record.compute_backend?.executed;
  if (executed) {
    const [state, label] = ENGINE_STATES[term.status] ?? ['failed', f.words(term.status)];
    stages.push(stage('engine', 'Engine', 'execution', state, label,
      [`executed: ${name(executed)}`, f.isAbsent(backend) ? null : `backend: ${backend.toUpperCase()}`], st.engine));
  } else if (d?.invoked && !noEngine) {
    // Chosen, but refused before it ran (e.g. an explicit CUDA request this
    // build cannot honour). The engine path's own time is shown if recorded.
    stages.push(stage('engine', 'Engine', 'execution', 'refused', 'not executed',
      [`selected: ${name(d.engine)}`, 'executed: none', term.message], st.engine, 'engine path'));
  } else {
    stages.push(notRun('engine', 'Engine', 'execution', 'not run', ['nothing executed']));
  }

  // POSTSOLVE -- reconstruction into original coordinates. invalid_mapping /
  // internal_error are the Postsolver's reconstruction failures; violation
  // statuses are verdicts of the validation it then performs (next stage).
  const original = v?.original_space ?? null;
  const reduced = v?.reduced_space ?? null;
  if (!original) stages.push(notRun('postsolve', 'Postsolve', 'validation'));
  else if (original.status === 'invalid_mapping' || original.status === 'internal_error') {
    stages.push(stage('postsolve', 'Postsolve', 'validation', 'failed', 'reconstruction failed', [original.failure], st.postsolve));
  } else {
    stages.push(stage('postsolve', 'Postsolve', 'validation', 'completed', 'completed', ['original coordinates'], st.postsolve));
  }

  // VALIDATION -- both checks, by space.
  const checks = [['reduced space', reduced], ['original space', original]].filter(([, check]) => check);
  if (!checks.length) stages.push(notRun('validation', 'Validation', 'validation'));
  else {
    const failed = checks.some(([, check]) => !check.passed);
    stages.push(stage('validation', 'Validation', 'validation', failed ? 'failed' : 'completed', failed ? 'failed' : 'passed',
      checks.map(([space, check]) => `${space}: ${check.passed ? 'passed' : f.words(check.status)}`),
      st.reduced_validation, 'reduced check'));
  }

  // RESULT -- a valid model the selected engine refused is not an "invalid model".
  const [resultState, resultLabel] = readDispatch(record).engineRejectedModel
    ? ['failed', 'engine rejected model']
    : RESULT_STATES[term.status] ?? ['failed', f.words(term.status)];
  stages.push(stage('result', 'Result', 'run', resultState, resultLabel,
    [f.isAbsent(record.objective) ? 'no point returned' : `objective ${f.real(record.objective)}`], st.total, 'total'));

  // Termination point: the last stage before Result that actually ran.
  const ran = stages.slice(0, -1).filter((s) => s.state !== 'not_run');
  const end = ran[ran.length - 1];
  end.terminal = true;
  // KAIRO's own explanation is in the headline and on the stage itself; the
  // summary only says where the run stopped.
  return {
    stages,
    terminatedAt: end.id,
    summary: resultState === 'completed'
      ? `Ran end to end: ${end.label.toLowerCase()} ${end.stateLabel}.`
      : `Ended at ${end.label}: ${end.stateLabel}.`,
  };
}

function seconds(stageView) {
  if (stageView.seconds === null) return null;
  return h('span', { class: 'stage-time mono' },
    f.seconds(stageView.seconds), stageView.timeLabel ? h('span', { class: 'stage-time-label' }, ` ${stageView.timeLabel}`) : null);
}

export function renderPipeline(pipeline) {
  return h('section', { class: 'pipeline', 'data-section': 'pipeline', 'aria-labelledby': 'h-pipeline' },
    h('div', { class: 'pipeline-head' },
      h('h2', { id: 'h-pipeline', class: 'panel-title' }, 'Solver pipeline'),
      h('p', { class: 'pipeline-summary', 'data-field': 'pipeline-summary' }, pipeline.summary)),
    h('ol', { class: 'pipeline-rail' },
      pipeline.stages.map((s) =>
        h('li', { class: `stage stage-${s.tone}${s.terminal ? ' stage-terminal' : ''}`,
                  'data-stage-id': s.id, 'data-stage-state': s.state },
          h('button', { type: 'button', class: 'stage-button', 'data-target': s.target,
                        title: `Show ${s.target} details` },
            h('span', { class: 'stage-name' }, s.label),
            h('span', { class: 'stage-state', 'data-field': 'stage-state' }, s.stateLabel),
            s.lines.map((line) => h('span', { class: 'stage-line' }, line)),
            seconds(s),
            s.terminal ? h('span', { class: 'stage-end' }, 'ended here') : null)))));
}
