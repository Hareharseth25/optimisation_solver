// End-to-end: real browser -> Explorer UI -> Explorer service -> optimsolver.
//
// Starts the Explorer service on a free port, drives headless Chrome through
// the DevTools Protocol (Node's built-in WebSocket; no npm dependencies),
// uploads repository MPS files through the actual file input, presses Run, and
// checks what the page shows. Optional: needs Chrome; run via
//
//   node explorer/web/tests/e2e/browser.e2e.js --binary build/optimsolver \
//        [--chrome /path/to/chrome] [--screenshots out/] [--color-scheme light|dark]
//
// or CTest with -DEXPLORER_BROWSER_TESTS=ON.

import { spawn } from 'node:child_process';
import { mkdtempSync, mkdirSync, rmSync, writeFileSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../../..');
const args = Object.fromEntries(process.argv.slice(2).reduce((pairs, arg, i, all) =>
  (arg.startsWith('--') ? [...pairs, [arg.slice(2), all[i + 1]]] : pairs), []));
const BINARY = path.resolve(args.binary ?? process.env.OPTIMSOLVER_BINARY ?? '');
const CHROME = args.chrome ?? process.env.CHROME_PATH ??
  '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
const PYTHON = process.env.PYTHON ?? 'python3';
const SHOTS = args.screenshots ? path.resolve(args.screenshots) : null;

if (!existsSync(BINARY)) throw new Error(`optimsolver binary not found: ${BINARY}`);
if (!existsSync(CHROME)) throw new Error(`Chrome not found: ${CHROME} (pass --chrome)`);

// Reads a line matching `pattern` from a child's stderr.
function waitForLine(child, pattern, what) {
  return new Promise((resolve, reject) => {
    let buffer = '';
    const timer = setTimeout(() => reject(new Error(`timed out waiting for ${what}`)), 20000);
    child.stderr.on('data', (chunk) => {
      buffer += chunk;
      const match = buffer.match(pattern);
      if (match) { clearTimeout(timer); resolve(match); }
    });
    child.on('exit', (code) => reject(new Error(`${what} exited early (${code}): ${buffer}`)));
  });
}

class Cdp {
  constructor(url) {
    this.socket = new WebSocket(url);
    this.nextId = 1;
    this.pending = new Map();
    this.listeners = [];
    this.socket.onmessage = (event) => {
      const message = JSON.parse(event.data);
      if (message.id && this.pending.has(message.id)) {
        const { resolve, reject } = this.pending.get(message.id);
        this.pending.delete(message.id);
        message.error ? reject(new Error(message.error.message)) : resolve(message.result);
      } else if (message.method) {
        for (const listener of this.listeners) listener(message);
      }
    };
  }
  open() { return new Promise((resolve, reject) => { this.socket.onopen = resolve; this.socket.onerror = reject; }); }
  send(method, params = {}, sessionId) {
    const id = this.nextId++;
    this.socket.send(JSON.stringify({ id, method, params, ...(sessionId ? { sessionId } : {}) }));
    return new Promise((resolve, reject) => this.pending.set(id, { resolve, reject }));
  }
  once(method) {
    return new Promise((resolve) => {
      const listener = (message) => {
        if (message.method === method) {
          this.listeners = this.listeners.filter((l) => l !== listener);
          resolve(message.params);
        }
      };
      this.listeners.push(listener);
    });
  }
}

const failures = [];
const check = (ok, what) => { if (!ok) failures.push(what); };
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

async function main() {
  const service = spawn(PYTHON, ['-m', 'kairo_explorer', '--binary', BINARY, '--port', '0'],
    { cwd: path.join(ROOT, 'explorer'), stdio: ['ignore', 'ignore', 'pipe'] });
  const profile = mkdtempSync(path.join(tmpdir(), 'kairo-e2e-'));
  let chrome;
  try {
    const [, base] = await waitForLine(service, /KAIRO Explorer on (http:\/\/[^\s]+)/, 'Explorer service');
    chrome = spawn(CHROME, ['--headless=new', '--remote-debugging-port=0', `--user-data-dir=${profile}`,
      '--no-first-run', '--no-default-browser-check', '--disable-gpu', 'about:blank'],
    { stdio: ['ignore', 'ignore', 'pipe'] });
    const [, wsUrl] = await waitForLine(chrome, /DevTools listening on (ws:\/\/\S+)/, 'Chrome');

    const cdp = new Cdp(wsUrl);
    await cdp.open();
    const { targetId } = await cdp.send('Target.createTarget', { url: 'about:blank' });
    const { sessionId } = await cdp.send('Target.attachToTarget', { targetId, flatten: true });
    const send = (method, params) => cdp.send(method, params, sessionId);
    const pageErrors = [];
    cdp.listeners.push((message) => {
      if (message.sessionId !== sessionId) return;
      if (message.method === 'Runtime.exceptionThrown') pageErrors.push(message.params.exceptionDetails.text);
      // Expected 4xx answers are logged by Chrome as network errors; those are not page errors.
      if (message.method === 'Log.entryAdded' && message.params.entry.level === 'error' &&
          message.params.entry.source !== 'network') {
        pageErrors.push(message.params.entry.text);
      }
    });
    await send('Page.enable');
    await send('Runtime.enable');
    await send('Log.enable');
    await send('DOM.enable');
    await send('Emulation.setDeviceMetricsOverride', { width: 1440, height: 1000, deviceScaleFactor: 1, mobile: false });
    if (args['color-scheme']) {
      await send('Emulation.setEmulatedMedia', { features: [{ name: 'prefers-color-scheme', value: args['color-scheme'] }] });
    }

    const evaluate = async (expression) => {
      const { result, exceptionDetails } = await send('Runtime.evaluate',
        { expression, returnByValue: true, awaitPromise: true });
      if (exceptionDetails) throw new Error(`${expression}: ${exceptionDetails.text}`);
      return result.value;
    };
    const waitFor = async (expression, what, timeout = 30000) => {
      const start = Date.now();
      while (Date.now() - start < timeout) {
        if (await evaluate(expression)) return;
        await sleep(50);
      }
      throw new Error(`timed out waiting for ${what}`);
    };
    const screenshot = async (name) => {
      if (!SHOTS) return;
      mkdirSync(SHOTS, { recursive: true });
      const { cssContentSize } = await send('Page.getLayoutMetrics');
      const { data } = await send('Page.captureScreenshot', { format: 'png', captureBeyondViewport: true,
        clip: { x: 0, y: 0, width: 1440, height: Math.ceil(cssContentSize.height), scale: 1 } });
      writeFileSync(path.join(SHOTS, `${name}.png`), Buffer.from(data, 'base64'));
    };

    const load = async () => {
      const loaded = cdp.once('Page.loadEventFired');
      await send('Page.navigate', { url: base });
      await loaded;
      await waitFor(`document.getElementById('service-status').dataset.state !== 'checking'`, 'health check');
    };

    // Idle page: run disabled until a model is loaded.
    await load();
    check(await evaluate(`document.getElementById('service-status').textContent`) ===
      'Service connected · optimsolver.solve.v1', 'service health shown');
    check(await evaluate(`document.getElementById('run-button').disabled`), 'Run disabled without a model');
    check((await evaluate(`document.querySelector('[data-view=idle]')?.textContent`)).includes('No run yet'), 'idle state');
    await screenshot('00-idle');

    const solve = async (name, file, options = {}) => {
      await load();
      const { root } = await send('DOM.getDocument', {});
      const { nodeId } = await send('DOM.querySelector', { nodeId: root.nodeId, selector: '#model-file' });
      await send('DOM.setFileInputFiles', { nodeId, files: [path.join(ROOT, file)] });
      await waitFor(`document.getElementById('model-state').dataset.state === 'ready'`, `${name} file ready`);
      const fileState = await evaluate(`document.getElementById('model-state').textContent`);
      check(fileState.startsWith(path.basename(file)) && fileState.endsWith('ready'), `${name}: file state "${fileState}"`);
      for (const [id, value] of Object.entries(options)) {
        await evaluate(`(() => { const el = document.getElementById(${JSON.stringify(id)});
          el.value = ${JSON.stringify(value)}; el.dispatchEvent(new Event('input', {bubbles: true})); })()`);
      }
      check(!(await evaluate(`document.getElementById('run-button').disabled`)), `${name}: Run enabled`);
      // Click twice: the second click must not submit a second solve.
      const sawRunning = await evaluate(`(() => { const b = document.getElementById('run-button');
        b.click(); const running = b.disabled && !!document.querySelector('[data-view=running]'); b.click(); return running; })()`);
      check(sawRunning, `${name}: Run disables itself and shows the running state`);
      const runningText = await evaluate(`document.querySelector('[data-view=running]')?.textContent ?? ''`);
      check(!/%/.test(runningText), `${name}: no fake progress (${runningText})`);
      await waitFor(`!!document.querySelector('[data-view=result], [data-view=error]')`, `${name} result`);
      check(!(await evaluate(`document.getElementById('run-button').disabled`)), `${name}: Run re-enabled`);
      const page = await evaluate(`(() => {
        const t = (sel) => document.querySelector(sel)?.innerText.replace(/\\s+/g, ' ').trim() ?? null;
        // innerText applies the labels' text-transform, so engine boxes compare lower-cased.
        const lower = (sel) => t(sel)?.toLowerCase() ?? null;
        return { summary: t('[data-view=summary]'), selected: lower('[data-role=selected-engine]'),
                 executed: lower('[data-role=executed-engine]'), objective: t('[data-field=objective]'),
                 problemClass: t('[data-field=problem-class]'),
                 sections: [...document.querySelectorAll('[data-section]')].map((s) => s.dataset.section),
                 notRun: document.querySelectorAll('[data-state=not-run]').length,
                 stages: [...document.querySelectorAll('[data-stage]')].map((s) => s.dataset.stage) };
      })()`);
      await screenshot(name);
      return page;
    };

    let page = await solve('01-lp', 'tests/cli/presolve_reduction.mps');
    check(page.summary.startsWith('Optimal'), `LP summary: ${page.summary}`);
    check(page.objective === '28' && page.problemClass === 'LP', 'LP objective/class');
    check(page.selected === 'selected by dispatcher dual simplex' && page.executed === 'executed dual simplex', 'LP engines');
    check(page.sections.join() === 'run,model,presolve,dispatch,execution,validation', `LP sections ${page.sections}`);
    check(page.notRun === 0, 'LP: every stage ran');

    page = await solve('02-netlib-afiro', 'benchmarks/instances/netlib/afiro.mps');
    check(page.summary.startsWith('Optimal') && page.objective === '-464.7531429', `afiro: ${page.objective}`);

    page = await solve('03-milp', 'tests/cli/knapsack_milp.mps', { threads: '1' });
    check(page.problemClass === 'MILP' && page.objective === '7', 'MILP');
    check(page.executed === 'executed branch and cut', 'MILP engine');

    page = await solve('04-qp', 'tests/cli/convex_qp.mps');
    check(page.problemClass === 'QP' && page.objective === '-4.5' && page.executed === 'executed qp', 'QP');

    page = await solve('05-presolve-infeasible', 'tests/mps/test_cases/01_basic_lp.mps');
    check(page.summary.includes('Proved by presolve. The dispatcher was not invoked and no engine ran.'), 'infeasible summary');
    check(page.executed === 'executed nothing executed', `infeasible executed: ${page.executed}`);
    check(page.stages.join() === 'validation,classification,presolve,total', `infeasible stages ${page.stages}`);
    check(page.notRun === 2, 'infeasible: both validations not run');

    page = await solve('06-unsupported', 'tests/cli/nonconvex_qp.mps');
    check(page.summary.startsWith('Unsupported') && page.summary.includes('HTTP 422'), `unsupported: ${page.summary}`);
    check(page.executed === 'executed nothing executed', 'unsupported: nothing executed');

    page = await solve('07-invalid-model', 'tests/cli/invalid_bounds.mps');
    check(page.summary.startsWith('Invalid model') && page.summary.includes('HTTP 422'), `invalid: ${page.summary}`);
    check(page.selected === null && page.notRun >= 4, 'invalid: solver sections not run');

    page = await solve('08-forced-pdlp-cpu', 'tests/cli/simple_lp.mps', { engine: 'pdlp', backend: 'cpu' });
    check(page.selected === 'selected by dispatcher pdlp' && page.executed === 'executed pdlp', 'forced pdlp');

    page = await solve('09-rejected-option', 'tests/cli/simple_lp.mps', { threads: '-1' });
    check(page.summary.startsWith('KAIRO rejected an option') && page.summary.includes('Invalid thread count'),
      `rejected: ${page.summary}`);

    check(pageErrors.length === 0, `no page errors or CSP violations: ${pageErrors.join(' | ')}`);
    cdp.socket.close();
  } finally {
    chrome?.kill();
    service.kill();
    await sleep(200);
    rmSync(profile, { recursive: true, force: true });
  }
  if (failures.length) {
    for (const failure of failures) console.error(`FAIL ${failure}`);
    process.exit(1);
  }
  console.log('Explorer browser end-to-end: all checks passed');
}

main().catch((error) => { console.error(error); process.exit(1); });
