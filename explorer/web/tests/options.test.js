import { test } from 'node:test';
import assert from 'node:assert/strict';
import { ENGINES, BACKENDS, DEFAULT_FORM, fieldStates, validate, buildOptions } from '../js/options.js';
import { buildRequest } from '../js/api.js';

const file = { name: 'm.mps', size: 10, text: 'NAME X', ready: true };

test('engine and backend choices are the service options, automatic first', () => {
  assert.equal(ENGINES[0].value, 'auto');
  assert.deepEqual(ENGINES.map((e) => e.value).slice(1),
    ['dual_simplex', 'pdlp', 'barrier', 'branch_and_cut', 'qp', 'miqp']);
  assert.deepEqual(BACKENDS.map((b) => b.value), ['auto', 'cpu', 'cuda']);
});

test('defaults send only the engine and backend, leaving the rest to KAIRO', () => {
  assert.deepEqual(buildOptions(DEFAULT_FORM), { engine: 'auto', backend: 'auto' });
});

test('every option is sent in the service format', () => {
  const form = { engine: 'pdlp', backend: 'cuda', timeLimit: '2.5', threads: '4', cudaDevice: '1' };
  assert.deepEqual(buildOptions(form),
    { engine: 'pdlp', backend: 'cuda', time_limit_seconds: 2.5, threads: 4, cuda_device: 1 });
  assert.deepEqual(buildRequest('NAME X', buildOptions(form)),
    { model: { format: 'mps', content: 'NAME X' },
      options: { engine: 'pdlp', backend: 'cuda', time_limit_seconds: 2.5, threads: 4, cuda_device: 1 } });
});

test('backend and CUDA device are disabled where they do not apply, and not sent', () => {
  const simplex = { ...DEFAULT_FORM, engine: 'dual_simplex', backend: 'cuda', cudaDevice: '2' };
  const states = fieldStates(simplex);
  assert.equal(states.backend.enabled, false);
  assert.match(states.backend.note, /no CUDA backend/);
  assert.equal(states.cudaDevice.enabled, false);
  assert.deepEqual(buildOptions(simplex), { engine: 'dual_simplex' });

  const cpu = { ...DEFAULT_FORM, engine: 'pdlp', backend: 'cpu', cudaDevice: '2' };
  assert.equal(fieldStates(cpu).backend.enabled, true);
  assert.equal(fieldStates(cpu).cudaDevice.enabled, false);
  assert.deepEqual(buildOptions(cpu), { engine: 'pdlp', backend: 'cpu' });
});

test('only basic UI requirements are validated; values are left to KAIRO', () => {
  assert.deepEqual(validate(DEFAULT_FORM, null).map((e) => e.field), ['model']);
  assert.deepEqual(validate(DEFAULT_FORM, file), []);
  const bad = { ...DEFAULT_FORM, timeLimit: 'soon', threads: '2.5', cudaDevice: 'gpu0' };
  assert.deepEqual(validate(bad, file).map((e) => e.field), ['timeLimit', 'threads', 'cudaDevice']);
  // A negative limit is well-formed: KAIRO, not the form, decides it is invalid.
  assert.deepEqual(validate({ ...DEFAULT_FORM, timeLimit: '-5', threads: '-1' }, file), []);
});
