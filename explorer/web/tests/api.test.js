import { test } from 'node:test';
import assert from 'node:assert/strict';
import { postSolve, createRunner, checkHealth, SOLVE_URL } from '../js/api.js';
import { fixture } from './helpers.js';

function fakeFetch(status, body, calls = []) {
  return async (url, init) => {
    calls.push({ url, init });
    return { status, ok: status < 400, json: async () => (typeof body === 'function' ? body() : body) };
  };
}

test('run submission posts the request JSON to /api/solve', async () => {
  const calls = [];
  const request = { model: { format: 'mps', content: 'NAME X' }, options: { engine: 'auto' } };
  const lp = fixture('lp_optimal');
  const result = await postSolve(request, fakeFetch(200, lp.body, calls));
  assert.equal(calls.length, 1);
  assert.equal(calls[0].url, SOLVE_URL);
  assert.equal(calls[0].init.method, 'POST');
  assert.equal(calls[0].init.headers['Content-Type'], 'application/json');
  assert.deepEqual(JSON.parse(calls[0].init.body), request);
  assert.deepEqual(result, { kind: 'response', httpStatus: 200, body: lp.body });
});

test('error statuses are returned as responses, never turned into success', async () => {
  const invalid = fixture('invalid_model');
  const result = await postSolve({}, fakeFetch(422, invalid.body));
  assert.equal(result.kind, 'response');
  assert.equal(result.httpStatus, 422);
  assert.equal(result.body.outcome, 'invalid_model');
});

test('unreachable service and non-JSON answers are transport failures', async () => {
  const down = await postSolve({}, async () => { throw new TypeError('fetch failed'); });
  assert.equal(down.kind, 'transport');
  assert.match(down.message, /could not be reached/);
  const html = await postSolve({}, fakeFetch(502, () => { throw new SyntaxError('bad'); }));
  assert.equal(html.kind, 'transport');
  assert.match(html.message, /HTTP 502/);
  const odd = await postSolve({}, fakeFetch(200, { hello: 1 }));
  assert.equal(odd.kind, 'transport');
});

test('duplicate submission is refused while a solve is running', async () => {
  let release;
  const calls = [];
  const slow = async (url, init) => {
    calls.push(init);
    await new Promise((resolve) => { release = resolve; });
    return { status: 200, ok: true, json: async () => fixture('lp_optimal').body };
  };
  const runner = createRunner(slow);
  const first = runner.run({});
  assert.equal(runner.busy, true);
  assert.equal(await runner.run({}), null, 'second run refused');
  release();
  assert.equal((await first).kind, 'response');
  assert.equal(runner.busy, false);
  assert.equal(calls.length, 1);
});

test('health reports the contract', async () => {
  assert.deepEqual(await checkHealth(fakeFetch(200, { status: 'ok', contract: 'optimsolver.solve.v1' })),
    { ok: true, contract: 'optimsolver.solve.v1' });
  assert.deepEqual(await checkHealth(async () => { throw new Error('down'); }), { ok: false });
});
