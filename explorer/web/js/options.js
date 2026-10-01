// Solver options the Explorer service accepts (explorer/README.md), and the
// small UI rules around them. KAIRO stays authoritative for VALUES: the form
// only checks that a number is a number; whether a time limit or thread count
// is acceptable is decided by KAIRO and shown if it says no.

// Engine names KAIRO's dispatcher accepts for MPS models (solver::parseEngine,
// canonical spellings). `cuda` marks the engines with a CUDA backend
// (solver::ComputeBackend, docs/cuda.md); the others always run on the CPU.
// explorer/tests/test_service.py checks each name against the real binary.
export const ENGINES = [
  { value: 'auto', label: 'Automatic (dispatcher decides)', cuda: true },
  { value: 'dual_simplex', label: 'Dual simplex', cuda: false },
  { value: 'pdlp', label: 'PDLP', cuda: true },
  { value: 'barrier', label: 'Barrier (interior point)', cuda: false },
  { value: 'branch_and_cut', label: 'Branch and cut', cuda: false },
  { value: 'qp', label: 'QP (ADMM)', cuda: true },
  { value: 'miqp', label: 'MIQP (branch and bound)', cuda: false },
];

export const BACKENDS = [
  { value: 'auto', label: 'Auto (CUDA when available)' },
  { value: 'cpu', label: 'CPU' },
  { value: 'cuda', label: 'CUDA' },
];

export const DEFAULT_FORM = Object.freeze({
  engine: 'auto', backend: 'auto', timeLimit: '', threads: '', cudaDevice: '',
});

// Which fields apply to the current choices.
export function fieldStates(form) {
  const engine = ENGINES.find((e) => e.value === form.engine) ?? ENGINES[0];
  const backend = engine.cuda;
  const cudaDevice = backend && form.backend !== 'cpu';
  return {
    backend: {
      enabled: backend,
      note: backend ? null : `${engine.label} has no CUDA backend; it runs on the CPU.`,
    },
    cudaDevice: {
      enabled: cudaDevice,
      note: cudaDevice ? null : 'Only used with the CUDA or Auto backend.',
    },
  };
}

const isBlank = (text) => String(text ?? '').trim() === '';

// Basic UI requirements only. Returns a list of {field, message}.
export function validate(form, file) {
  const errors = [];
  if (!file) errors.push({ field: 'model', message: 'Load an MPS file first.' });
  if (!isBlank(form.timeLimit) && !Number.isFinite(Number(form.timeLimit))) {
    errors.push({ field: 'timeLimit', message: 'Time limit must be a number of seconds.' });
  }
  if (!isBlank(form.threads) && !/^-?\d+$/.test(String(form.threads).trim())) {
    errors.push({ field: 'threads', message: 'Threads must be a whole number.' });
  }
  const states = fieldStates(form);
  if (states.cudaDevice.enabled && !isBlank(form.cudaDevice) &&
      !/^-?\d+$/.test(String(form.cudaDevice).trim())) {
    errors.push({ field: 'cudaDevice', message: 'CUDA device must be a whole number.' });
  }
  return errors;
}

// Form -> the service's `options` object. Blank fields are left to KAIRO's
// defaults; disabled fields are not sent at all.
export function buildOptions(form) {
  const states = fieldStates(form);
  const options = { engine: form.engine };
  if (states.backend.enabled) options.backend = form.backend;
  if (!isBlank(form.timeLimit)) options.time_limit_seconds = Number(form.timeLimit);
  if (!isBlank(form.threads)) options.threads = Number.parseInt(form.threads, 10);
  if (states.cudaDevice.enabled && !isBlank(form.cudaDevice)) {
    options.cuda_device = Number.parseInt(form.cudaDevice, 10);
  }
  return options;
}
