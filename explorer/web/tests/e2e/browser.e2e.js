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
      await send('DOM.setFileInputFiles', { nodeId, files: [path.isAbsolute(file) ? file : path.join(ROOT, file)] });
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
                 pipeline: [...document.querySelectorAll('[data-stage-id]')]
                   .map((li) => li.dataset.stageId + ':' + li.dataset.stageState).join(' '),
                 terminal: document.querySelector('.stage-terminal')?.dataset.stageId ?? null,
                 pipelineSummary: t('[data-field=pipeline-summary]'),
                 analysisClass: t('[data-field=analysis-class]'),
                 headline: t('.banner-title'),
                 requestMode: t('[data-field=request-mode]'),
                 invoked: t('[data-field=invoked]'),
                 reason: t('[data-field=reason]'),
                 refusal: t('[data-field=refusal]'),
                 refusalMessage: t('[data-field=refusal-message]'),
                 executionEngine: t('[data-field=execution-engine]'),
                 executionBackend: t('[data-field=execution-backend]'),
                 engineStageLabel: t('[data-stage=engine] th'),
                 analysisSense: t('[data-field=analysis-sense]'),
                 analysisVariables: t('[data-field=analysis-variables]'),
                 composition: t('[data-field=analysis-composition]'),
                 structure: t('[data-field=analysis-structure]'),
                 analysisText: t('[data-section=model-analysis]'),
                 impactState: t('[data-field=impact-state]'),
                 impactHeading: t('[data-field=impact-heading]'),
                 impactRows: [...document.querySelectorAll('[data-impact]')]
                   .map((tr) => [...tr.children].map((c) => c.textContent.trim()).join('|')),
                 impactTime: t('[data-field=impact-time]'),
                 transformations: t('[data-field=impact-transformations]'),
                 impactText: t('[data-section=presolve-impact]'),
                 stages: [...document.querySelectorAll('[data-stage]')].map((s) => s.dataset.stage) };
      })()`);
      await screenshot(name);
      return page;
    };

    let page = await solve('01-lp', 'tests/cli/presolve_reduction.mps');
    check(page.summary.startsWith('Optimal'), `LP summary: ${page.summary}`);
    check(page.objective === '28' && page.problemClass === 'LP', 'LP objective/class');
    check(page.selected === 'selected engine dual simplex' && page.executed === 'executed dual simplex', 'LP engines');
    check(page.requestMode === 'Automatic — the dispatcher chooses' && page.invoked === 'Invoked' &&
      page.reason.startsWith('small enough for the dual simplex'), `LP dispatch: ${page.requestMode} / ${page.reason}`);
    check(page.engineStageLabel === 'Engine execution' && page.executionEngine === 'dual simplex',
      `LP execution: ${page.engineStageLabel} / ${page.executionEngine}`);
    check(page.refusal === null, 'LP: no refusal block');
    check(page.notRun === 0, 'LP: every stage ran');
    const ALL_DONE = 'model:completed classification:completed presolve:completed dispatch:completed ' +
      'engine:completed postsolve:completed validation:completed result:completed';
    check(page.pipeline === ALL_DONE, `LP pipeline: ${page.pipeline}`);
    check(page.terminal === 'validation' && page.pipelineSummary === 'Ran end to end: validation passed.',
      `LP termination: ${page.terminal} / ${page.pipelineSummary}`);
    check(page.sections.join() ===
      'pipeline,model-analysis,presolve-impact,run,model,presolve,dispatch,execution,validation',
      `pipeline, analysis, then the evidence: ${page.sections}`);
    check(page.analysisClass === 'LP' && page.analysisSense === 'Minimize' && page.analysisVariables === '3',
      `LP analysis: ${page.analysisClass} ${page.analysisSense} ${page.analysisVariables}`);
    check(page.composition === 'Variable composition 3 continuous 0 integer 0 binary', `LP composition: ${page.composition}`);
    check(page.impactState === 'Converged' && page.impactHeading === 'Original → reduced', 'LP impact state');
    check(page.impactRows.join(' / ') === 'Variables|3|2|−1|33.3 % / Constraints|1|1|0|0 % / Nonzeros|3|2|−1|33.3 %',
      `LP impact rows: ${page.impactRows.join(' / ')}`);
    check(/^presolve time \d/.test(page.impactTime ?? ''), `LP presolve time: ${page.impactTime}`);
    check(page.transformations === '1 transformation logged fix_variable ×1', `LP transformations: ${page.transformations}`);

    // Clicking a stage moves to its detailed section.
    for (const [stageId, sectionName] of [['dispatch', 'dispatch'], ['presolve', 'presolve'], ['engine', 'execution']]) {
      const landed = await evaluate(`(() => {
        document.querySelector('[data-stage-id=${stageId}] button').click();
        const section = document.querySelector('[data-section=${sectionName}]');
        return section.classList.contains('is-highlighted') && section.contains(document.activeElement) &&
               document.querySelectorAll('.is-highlighted').length === 1; })()`);
      check(landed, `clicking ${stageId} highlights and focuses ${sectionName}`);
    }
    await screenshot('01b-lp-dispatch-highlighted');

    page = await solve('02-netlib-afiro', 'benchmarks/instances/netlib/afiro.mps');
    check(page.summary.startsWith('Optimal') && page.objective === '-464.7531429', `afiro: ${page.objective}`);

    page = await solve('03-milp', 'tests/cli/knapsack_milp.mps', { threads: '1' });
    check(page.problemClass === 'MILP' && page.objective === '7', 'MILP');
    check(page.analysisClass === 'MILP' && page.analysisSense === 'Maximize' &&
      page.composition === 'Variable composition 0 continuous 0 integer 3 binary', `MILP analysis: ${page.composition}`);
    check(page.impactRows.every((row) => row.endsWith('|0|0 %')), `MILP unchanged: ${page.impactRows}`);
    check(page.executed === 'executed branch and cut', 'MILP engine');

    page = await solve('04-qp', 'tests/cli/convex_qp.mps');
    check(page.problemClass === 'QP' && page.objective === '-4.5' && page.executed === 'executed qp', 'QP');
    check(page.analysisClass === 'QP' && page.transformations === '2 transformations logged tighten_upper_bound ×2',
      `QP tightening only: ${page.transformations}`);
    check(!/removed/i.test(page.impactText), 'QP: tightenings not called removals');

    page = await solve('05-presolve-infeasible', 'tests/mps/test_cases/01_basic_lp.mps');
    check(page.summary.includes('Proved by presolve. The dispatcher was not invoked and no engine ran.'), 'infeasible summary');
    check(page.executed === 'executed nothing executed', `infeasible executed: ${page.executed}`);
    check(page.stages.join() === 'validation,classification,presolve,total', `infeasible stages ${page.stages}`);
    check(page.notRun === 2, 'infeasible: both validations not run');
    check(page.pipeline === 'model:completed classification:completed presolve:infeasible dispatch:not_run ' +
      'engine:not_run postsolve:not_run validation:not_run result:infeasible', `infeasible pipeline: ${page.pipeline}`);
    check(page.terminal === 'presolve', 'infeasible ends at presolve');
    check(page.impactState === 'Proved infeasible' && page.impactHeading === 'Dimensions when presolve stopped',
      `infeasible impact: ${page.impactState} / ${page.impactHeading}`);
    check(!page.impactText.includes('%') && page.impactRows.join(' / ') ===
      'Variables|2|2|0 / Constraints|3|1|−2 / Nonzeros|4|2|−2', `infeasible: no reductions: ${page.impactRows}`);

    page = await solve('06-unsupported', 'tests/cli/nonconvex_qp.mps');
    check(page.headline === 'No suitable engine' && page.summary.includes('HTTP 422'), `unsupported: ${page.summary}`);
    check(page.executed === 'executed nothing executed', 'unsupported: nothing executed');
    check(page.pipeline.includes('dispatch:unsupported engine:not_run') && page.terminal === 'dispatch',
      `unsupported pipeline: ${page.pipeline}`);

    page = await solve('07-invalid-model', 'tests/cli/invalid_bounds.mps');
    check(page.summary.startsWith('Invalid model') && page.summary.includes('HTTP 422'), `invalid: ${page.summary}`);
    check(page.selected === null && page.notRun >= 4, 'invalid: solver sections not run');
    check(page.invoked === 'Not reached' && page.executionEngine === 'nothing executed', 'invalid: dispatcher not reached');
    check(page.pipeline === 'model:failed classification:not_run presolve:not_run dispatch:not_run ' +
      'engine:not_run postsolve:not_run validation:not_run result:failed', `invalid pipeline: ${page.pipeline}`);
    check(page.terminal === 'model', 'invalid ends at model');
    check(page.analysisClass === null && page.analysisText.includes('Classification not run — KAIRO did not accept the model'),
      `invalid analysis: ${page.analysisText}`);
    check(page.impactState === 'Not run — the model never reached presolve' && page.impactTime === null,
      `invalid impact: ${page.impactState}`);

    // Unreadable: written to a temporary file and uploaded like any other.
    const unreadable = path.join(profile, 'unreadable.mps');
    writeFileSync(unreadable, 'NAME          BAD\nROWS\n N  OBJ\n Q  C1\nCOLUMNS\n    X  OBJ  1.0\nENDATA\n');
    page = await solve('07b-unreadable-model', unreadable);
    check(page.summary.startsWith('Invalid model') && page.analysisText.includes('Model not available — KAIRO could not read the file.'),
      `unreadable analysis: ${page.analysisText}`);
    check(page.pipeline.startsWith('model:failed') && page.impactState.startsWith('Not run'), 'unreadable: nothing ran');

    page = await solve('08-forced-pdlp-cpu', 'tests/cli/simple_lp.mps', { engine: 'pdlp', backend: 'cpu' });
    check(page.selected === 'selected engine pdlp' && page.executed === 'executed pdlp', 'forced pdlp');
    check(page.pipeline === ALL_DONE, `forced pipeline: ${page.pipeline}`);
    check(page.requestMode === 'Forced by caller: pdlp' && page.reason === 'engine forced by the caller', 'forced request');

    // Forcing barrier from the menu (the alias "ipm" is covered by the real
    // forced_alias fixture; the menu only offers canonical names).
    page = await solve('08b-forced-barrier', 'tests/cli/simple_lp.mps', { engine: 'barrier' });
    check(page.selected === 'selected engine barrier' && page.executed === 'executed barrier', 'forced barrier');

    // A valid MILP forced onto PDLP: the engine refuses; the model is not invalid.
    page = await solve('08c-forced-incompatible', 'tests/cli/knapsack_milp.mps', { engine: 'pdlp' });
    check(page.headline === 'Requested engine cannot solve this model', `forced incompatible headline: ${page.headline}`);
    check(page.summary.includes('invalid_model · HTTP 422'), 'KAIRO status still shown');
    check(page.selected === 'selected engine pdlp' && page.executed === 'executed nothing executed', 'incompatible engines');
    check(page.refusal.startsWith('Engine rejected the model') && page.refusalMessage.includes('integer variables'),
      `incompatible refusal: ${page.refusal}`);
    check(page.engineStageLabel === 'Engine path (no engine executed)', `engine path label: ${page.engineStageLabel}`);
    check(page.pipeline.startsWith('model:completed classification:completed'), 'model was accepted');

    // Selected but not executed: this build has no CUDA backend, so an explicit
    // CUDA request is refused after dispatch chose PDLP.
    page = await solve('10-cuda-refused', 'tests/cli/simple_lp.mps', { engine: 'pdlp', backend: 'cuda' });
    // Decided on the structured outcome, never on headline wording: on a build
    // without CUDA nothing executes; on a CUDA build PDLP must run on the GPU.
    if (page.executed === 'executed nothing executed') {
      check(page.pipeline === 'model:completed classification:completed presolve:completed dispatch:completed ' +
        'engine:refused postsolve:not_run validation:not_run result:unsupported', `cuda pipeline: ${page.pipeline}`);
      check(page.terminal === 'engine' && page.pipelineSummary === 'Ended at Engine: not executed.',
        `cuda termination: ${page.pipelineSummary}`);
      check(page.headline === 'Requested backend unavailable' && page.refusal.startsWith('Backend refusal') &&
        page.reason === 'engine forced by the caller' && page.executionBackend === 'requested cuda (device 0) → executed none' &&
        page.engineStageLabel === 'Engine path (no engine executed)', `cuda refusal: ${page.headline} / ${page.refusal}`);
      check(page.selected === 'selected engine pdlp', 'cuda: selected pdlp, nothing executed');
    } else {
      check(page.executed === 'executed pdlp' && page.executionBackend.includes('executed cuda'),
        `cuda build: pdlp must run on the GPU (${page.executionBackend})`);
    }

    page = await solve('11-engine-infeasible', 'tests/cli/engine_infeasible_lp.mps');
    check(page.pipeline === 'model:completed classification:completed presolve:completed dispatch:completed ' +
      'engine:infeasible postsolve:not_run validation:not_run result:infeasible', `engine infeasible: ${page.pipeline}`);
    check(page.terminal === 'engine', 'engine-infeasible ends at engine');

    page = await solve('13-structured-milp', 'tests/cli/structured_milp.mps');
    check(page.selected === 'dispatch outcome trivial path the solution is read from variable bounds; no iterative engine runs.' &&
      page.executed === 'executed trivial path', `trivial: ${page.selected} / ${page.executed}`);
    check(page.objective === '12' && /Big-M rows Detected · max 1000/.test(page.structure) &&
      /Set partitioning Detected/.test(page.structure) && /Symmetric column groups Detected · 1/.test(page.structure),
      `structure flags: ${page.structure}`);
    check(page.impactRows.join(' / ') === 'Variables|4|1|−3|75 % / Constraints|3|0|−3|100 % / Nonzeros|6|0|−6|100 %',
      `structured impact: ${page.impactRows}`);

    page = await solve('14-network-no-reduction', 'tests/cli/network_flow_lp.mps');
    check(page.objective === '4' && /Network structure Detected/.test(page.structure), `network: ${page.structure}`);
    check(page.impactRows.every((row) => row.endsWith('|0|0 %')) &&
      page.transformations === '3 transformations logged tighten_upper_bound ×3', `network impact: ${page.transformations}`);

    page = await solve('15-no-constraints', 'tests/cli/no_constraints_lp.mps');
    check(page.impactRows.join(' / ') === 'Variables|2|2|0|0 % / Constraints|0|0|0|— / Nonzeros|0|0|0|—',
      `zero denominators: ${page.impactRows}`);
    check(!/NaN|Infinity/.test(page.impactText), 'no NaN/Infinity');

    page = await solve('12-unbounded', 'tests/cli/unbounded_lp.mps');
    check(page.pipeline.includes('engine:unbounded postsolve:not_run validation:not_run result:unbounded'),
      `unbounded: ${page.pipeline}`);

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
