// DOM wiring only: reads the form, calls the service once per Run, and
// mounts the trees from report.js. No solver concept is computed here.

import { h, toDom } from './vdom.js';
import { ENGINES, BACKENDS, DEFAULT_FORM, fieldStates, validate, buildOptions } from './options.js';
import { buildRequest, createRunner, checkHealth } from './api.js';
import { renderIdle, renderRunning, renderResult } from './report.js';
import * as f from './format.js';

const $ = (id) => document.getElementById(id);
const runner = createRunner();
const state = { file: null };

const FIELDS = { engine: 'engine', backend: 'backend', timeLimit: 'time-limit', threads: 'threads', cudaDevice: 'cuda-device' };

function mount(target, node) {
  target.replaceChildren(toDom(node));
}

function readForm() {
  const form = {};
  for (const [key, id] of Object.entries(FIELDS)) form[key] = $(id).value;
  return form;
}

function populate(select, choices, value) {
  select.replaceChildren(...choices.map((c) => toDom(h('option', { value: c.value }, c.label))));
  select.value = value;
}

function sync() {
  const states = fieldStates(readForm());
  $('backend').disabled = !states.backend.enabled;
  $('backend-note').textContent = states.backend.note ?? '';
  $('cuda-device').disabled = !states.cudaDevice.enabled;
  $('cuda-note').textContent = states.cudaDevice.note ?? '';
  const button = $('run-button');
  button.disabled = !state.file?.ready || runner.busy;
  button.textContent = runner.busy ? 'Running…' : 'Run';
  button.setAttribute('aria-busy', runner.busy ? 'true' : 'false');
}

function showModelState(kind, text) {
  const box = $('model-state');
  box.dataset.state = kind;
  box.textContent = text;
}

async function onFile(event) {
  const file = event.target.files[0];
  state.file = null;
  if (!file) {
    showModelState('empty', 'No model loaded');
    sync();
    return;
  }
  showModelState('reading', `Reading ${file.name}…`);
  sync();
  try {
    const text = await file.text();
    if (!text.trim()) {
      showModelState('error', `${file.name} is empty.`);
    } else {
      state.file = { name: file.name, size: file.size, text, ready: true };
      showModelState('ready', `${file.name} · ${f.bytes(file.size)} · ready`);
    }
  } catch {
    showModelState('error', `${file.name} could not be read.`);
  }
  sync();
}

function showErrors(errors) {
  for (const id of Object.values(FIELDS)) $(id).removeAttribute('aria-invalid');
  for (const error of errors) if (FIELDS[error.field]) $(FIELDS[error.field]).setAttribute('aria-invalid', 'true');
  mount($('form-errors'), h('ul', {}, errors.map((e) => h('li', {}, e.message))));
}

async function onSubmit(event) {
  event.preventDefault();
  if (runner.busy) return;
  const form = readForm();
  const errors = validate(form, state.file?.ready ? state.file : null);
  showErrors(errors);
  if (errors.length) return;

  const file = state.file;
  const pending = runner.run(buildRequest(file.text, buildOptions(form)));
  sync();
  mount($('results'), renderRunning(file.name));
  const result = await pending;
  sync();
  if (result) mount($('results'), renderResult(result, { fileName: file.name, fileSize: file.size }));
}

async function showHealth() {
  const box = $('service-status');
  const health = await checkHealth();
  box.dataset.state = health.ok ? 'ok' : 'down';
  box.textContent = health.ok ? `Service connected · ${health.contract}` : 'Service unreachable';
}

function init() {
  populate($('engine'), ENGINES, DEFAULT_FORM.engine);
  populate($('backend'), BACKENDS, DEFAULT_FORM.backend);
  $('model-file').addEventListener('change', onFile);
  for (const id of Object.values(FIELDS)) $(id).addEventListener('input', sync);
  $('run-form').addEventListener('submit', onSubmit);
  mount($('results'), renderIdle());
  sync();
  showHealth();
}

init();
