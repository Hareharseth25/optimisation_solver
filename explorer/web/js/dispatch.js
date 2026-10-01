// Reading the dispatch decision and its execution out of the record.
//
// This is NOT dispatcher policy. Nothing here decides which engine fits a
// model or explains why: dispatch.reason is KAIRO's own sentence and is shown
// verbatim, never parsed. The reader only answers structural questions the
// record already settles field by field:
//
//   was the dispatcher reached / invoked?     dispatch (null = solver not called), dispatch.invoked
//   automatic or forced?                       settings.requested_engine (null = automatic)
//   what was selected?                         dispatch.engine
//   is that an outcome rather than an engine?  dispatch.engine in PSEUDO_ENGINES
//   what executed?                             dispatch.executed_engine (null = nothing)
//   selected but not executed -- why?          termination.message (+ compute_backend.reason)
//   engine time: execution or only the path?   stage_seconds.engine with executed_engine set or null
//
// dispatch.* is authoritative. The termination.dispatched_engine /
// executed_engine / engine_reason copies carry default values when no solve
// ran, so they are never read here.

// solver::Engine values that are dispatch OUTCOMES, not solver engines.
// Wording follows the enum's own documentation.
export const PSEUDO_ENGINES = {
  infeasible: { label: 'infeasible', title: 'Early exit', note: 'Presolve settled the solve; no engine is needed.' },
  trivial: { label: 'trivial path', title: 'Trivial path', note: 'The solution is read from variable bounds; no iterative engine runs.' },
  unsupported: { label: 'no suitable engine', title: 'No suitable engine', note: 'No KAIRO engine can solve the model as posed.' },
};

export const isPseudoEngine = (engine) => Object.prototype.hasOwnProperty.call(PSEUDO_ENGINES, engine);

const present = (value) => value !== null && value !== undefined;

export function readDispatch(record) {
  const d = record.dispatch ?? null;
  const requested = record.settings?.requested_engine ?? null;
  const backend = record.compute_backend ?? {};
  const stages = record.stage_seconds ?? {};
  const status = record.termination?.status;

  const state = !d ? 'not_called' : d.invoked ? 'invoked' : 'not_invoked';
  const selected = d ? d.engine : null;
  const executed = d ? (d.executed_engine ?? null) : null;
  const pseudo = present(selected) && isPseudoEngine(selected) ? selected : null;

  // A real engine was selected by an invoked dispatcher, and nothing ran.
  const selectedNotExecuted = state === 'invoked' && !pseudo && present(selected) && !executed;
  // The CUDA request is a structured fact (compute_backend.requested), so a
  // backend refusal is recognised without reading any message text.
  const backendRefusal = selectedNotExecuted && backend.requested === 'cuda' && !present(backend.executed);
  // The model was accepted and classified, and the selected engine refused
  // it. KAIRO reports invalid_model here, but the MODEL is not invalid.
  const engineRejectedModel = selectedNotExecuted && status === 'invalid_model' && record.classification != null;

  return {
    state,
    mode: present(requested) ? 'forced' : 'automatic',
    requested,                              // the caller's own spelling, e.g. "ipm"
    selected,
    pseudo,                                 // null, or a PSEUDO_ENGINES key
    executed,
    reason: d && d.reason ? d.reason : null, // the dispatcher's sentence, verbatim
    selectedNotExecuted,
    backendRefusal,
    engineRejectedModel,
    refusal: selectedNotExecuted ? {
      message: record.termination?.message || null,
      // Shown separately only when it adds something to the message.
      backendReason: backendRefusal && backend.reason && backend.reason !== record.termination?.message
        ? backend.reason : null,
    } : null,
    // Engine time is execution time only if an engine executed; otherwise it
    // is the time spent on the engine path before it refused.
    engineSeconds: present(stages.engine) ? stages.engine : null,
    engineTimeKind: !present(stages.engine) ? null : executed ? 'execution' : 'path',
    dispatchSeconds: present(stages.dispatch) ? stages.dispatch : null,
    // The reduced model is what the dispatcher was given (it runs after presolve).
    reducedModel: state === 'invoked' && record.presolve ? {
      variables: record.presolve.reduced_variables,
      constraints: record.presolve.reduced_constraints,
      nonzeros: record.presolve.reduced_nonzeros,
    } : null,
    backend: {
      requested: backend.requested ?? null,
      executed: backend.executed ?? null,
      requestedDevice: backend.requested_device ?? null,
      executedDevice: backend.executed_device ?? null,
      reason: backend.reason || null,
      // An explicit cpu/cuda request that did not come true. "auto" resolving
      // to cpu is a resolution, not a mismatch.
      mismatch: (backend.requested === 'cpu' || backend.requested === 'cuda') && backend.executed !== backend.requested,
    },
  };
}
