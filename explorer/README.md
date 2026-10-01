# KAIRO Explorer

KAIRO Explorer is the observability interface for the KAIRO solver. You load an MPS model, run it through KAIRO, and see what the solver did: classification, presolve, dispatch, execution and validation.

```text
KAIRO Core          solver::solve(model, options, &report)   ← solver truth
   │                optimsolver solve <model> --json <record>
   ▼
Explorer service    explorer/kairo_explorer                   ← interaction / orchestration
   │                POST /api/solve → optimsolver.solve.v1 record
   ▼
Explorer UI         explorer/web                              ← presentation
```

- **Core owns solver truth.** Classification, presolve, dispatch, engines, validation and every number in the record come from one `solver::solve()` call inside the `optimsolver` binary.
- **The application owns interaction.** It checks the request's shape, gives the model to KAIRO, and maps KAIRO's own termination status to an HTTP outcome. It has no optimization logic, does not interpret option values, and never parses terminal text.
- **The UI owns presentation.** It renders the record and nothing else. It has no data model of its own and computes no solver fact.
- **The CLI is another consumer** of the same core and the same record.

No new dependencies: the service uses only the Python standard library (≥ 3.9). The UI is plain HTML, CSS and ES modules with no build step and no npm packages.

## Running it (local development)

```sh
cmake --build build --target optimsolver
cd explorer
python3 -m kairo_explorer --binary ../build/optimsolver
```

Then open **http://127.0.0.1:8765/**. The same process serves the UI and the API, so the browser talks to the API from the same origin and the service needs no CORS. Use `--port` to change the port. It binds to the loopback interface by default. There is no separate frontend server and no build step: edit `explorer/web/` and reload.

### Workflow

1. **Model:** choose an MPS file (`.mps`, or `.qps` with QUADOBJ/QMATRIX). The sidebar shows the file name, its size, and whether it is ready. The file's text is sent in the request body; no path ever leaves the browser.
2. **Solver:** pick the options below. Fields that do not apply are disabled. For example, the backend is disabled when the chosen engine has no CUDA backend, and the CUDA device is disabled when the backend is CPU.
3. **Run:** the button disables itself and the page says *Running KAIRO…*. There is no progress bar, because KAIRO reports nothing until it finishes.
4. **Report:** the result headline, then Run, Model, Presolve, Dispatch, Execution and Validation, all read from the returned record.

Supported input: MPS / free MPS text, as the `optimsolver` CLI reads it. Supported options: engine (automatic, dual simplex, PDLP, barrier, branch and cut, QP, MIQP), backend (auto, CPU, CUDA), time limit, threads, CUDA device.

## API

### `GET /api/health`
`200 {"status": "ok", "contract": "optimsolver.solve.v1"}`

### `POST /api/solve` (`Content-Type: application/json`)

```json
{
  "model":   {"format": "mps", "content": "NAME  EXAMPLE\nROWS\n ..."},
  "options": {"engine": "auto", "backend": "cpu", "time_limit_seconds": 10, "threads": 1, "cuda_device": 0}
}
```

`model.format` must be `"mps"`. All options are optional. Each maps to an existing CLI flag, and KAIRO decides whether the value is acceptable:

| option | CLI flag | meaning |
|---|---|---|
| `engine` | `--solver` | `"auto"` (default: the dispatcher decides) or an engine name KAIRO accepts |
| `backend` | `--backend` | `auto`, `cpu`, `cuda`, for the engines that have a CUDA backend |
| `time_limit_seconds` | `--time-limit` | KAIRO's own solve budget |
| `threads` | `--threads` | worker threads, `0` = auto |
| `cuda_device` | `--cuda-device` | GPU index when `backend` is `cuda`/`auto` |

Response:

```json
{
  "outcome": "completed",
  "record":  { "schema": "optimsolver.solve.v1", "...": "..." },
  "error":   null,
  "process": {"exit_code": 0, "wall_seconds": 0.02}
}
```

`record` is exactly the record KAIRO wrote; the application never edits it. `process` holds application-level facts about the run, not solver data.

| `outcome` | HTTP | when |
|---|---|---|
| `completed` | 200 | `termination.status` is `optimal`, `infeasible`, `unbounded` or `limit_reached`; all are answers |
| `invalid_model` | 422 | `invalid_model`: unreadable MPS (the reader's reason is in `termination.message`) or a structurally invalid model |
| `unsupported` | 422 | `unsupported`: no engine can solve it as posed, or an explicit CUDA request cannot be honoured |
| `solver_failure` | 500 | `numerical_failure` |
| `bad_request` | 400 | malformed request; nothing was run, `record` is null |
| `rejected` | 400 | KAIRO refused an option value before reading the model; `error.message` is its text, passed through unparsed |
| `timeout` | 504 | the application's wall guard expired (KAIRO's time limit + 30 s, else 600 s) |
| `solver_crashed` / `no_record` / `malformed_record` / `unexpected_record` | 500 / 502 | KAIRO did not produce a valid record |

Use `record.termination.status` for the solver's verdict. Presolve-proved infeasibility is `status: "infeasible"` with `presolve.infeasible: true` and `dispatch.invoked: false`.

## Frontend contract

A frontend renders one run from the `optimsolver.solve.v1` record. There is no second data model. `null` always means "did not run / does not exist", never zero.

```text
Run         instance.{path, sha256}, solver.{commit, build_type}, settings,
            stage_seconds.{parse, solve}, process.wall_seconds (envelope)
 ├── Model       classification.{problem_class, num_rows, num_columns, nonzeros,
 │               num_binary, num_integer, num_continuous, coef_range_ratio,
 │               has_network_structure, has_big_m, max_big_m,
 │               has_set_partitioning, symmetric_groups}, instance.objective_sense
 ├── Presolve    presolve.{original_*, reduced_*, original_nonzeros, reduced_nonzeros,
 │               infeasible, converged, transformations, transformations_by_type}
 ├── Dispatch    dispatch.{invoked, engine, reason, executed_engine}
 ├── Execution   compute_backend.{requested, executed, reason, *_device},
 │               termination.executed_engine, work.{iterations, nodes, solve_seconds},
 │               stage_seconds.{validation, classification, presolve, dispatch,
 │               engine, reduced_validation, postsolve, total}
 ├── Validation  validation.reduced_space / validation.original_space
 │               .{passed, status, failure, max_*_residual[_scaled], objective,
 │               engine_reported_objective | duals_requested},
 │               self_reported.{max_dual_residual, max_integrality_violation,
 │               integrality_respected}
 └── Result      termination.{status, message}, objective, primal, duals,
                 reduced_costs, duals_unavailable_reason, variable_names,
                 constraint_names, dual_bound, mip_gap (null: not computed yet)
```

## Security (local/development scope)

- No shell. KAIRO runs as an argument list with `shell=False`; input is never interpolated into a command.
- No client paths. The model is written as `model.mps` in a fresh private temporary directory, and KAIRO runs with that as its working directory. The client cannot name a file, a directory or a binary. The record's `instance.path` is just `model.mps`.
- Only the five options above are reachable. `--output`, `--dump-model`, `.nlp` routing and every other CLI mode are not.
- The binary path is fixed when the server starts.
- Requests are capped at 64 MiB.
- Loopback-only by default; there is no authentication, so do not expose it publicly.

## Not yet: live progress

A solve is request → response. See `docs/architecture.md` ("Explorer Application Boundary") for what live progress would require.

## UI structure (`explorer/web/`)

| file | role |
|---|---|
| `index.html`, `styles.css` | page shell and design |
| `js/app.js` | DOM wiring only: form, file reading, one request per Run |
| `js/options.js` | the five service options, which fields apply, basic number checks, request `options` object |
| `js/api.js` | `POST /api/solve` and `GET /api/health`; refuses a second submission while one runs |
| `js/report.js` | pure render functions over the `optimsolver.solve.v1` record |
| `js/format.js` | number, time and residual formatting; `null` → "Not run" / "Unknown", never 0 |
| `js/vdom.js` | tiny `h()` tree, mounted with `textContent` only (no `innerHTML`) |

The engine menu lists the names `solver::parseEngine` accepts for MPS models. `explorer_service` checks each one against the real binary.

## Tests

- `explorer/tests/test_service.py` (CTest `explorer_service`): runs every case through the service and directly through the binary, and requires identical records apart from wall-clock values. Also covers static file serving, path traversal and the UI's engine names.
- `explorer/web/tests/*.test.js` (CTest `explorer_ui`, or `npm test` in `explorer/web`): Node's built-in runner. Covers upload state, the options form, request format, duplicate-submit refusal, and rendering of real service responses. These fixtures come from `tests/fixtures/make_fixtures.py`: LP, MILP, QP, presolve-infeasible, unsupported, invalid, unreadable, limit, forced engine and rejected option.
- `explorer/web/tests/e2e/browser.e2e.js` (CTest `explorer_ui_browser`, enabled with `-DEXPLORER_BROWSER_TESTS=ON`): starts the service and drives headless Chrome through the DevTools Protocol. It uploads real MPS files through the file input, presses Run, checks the page, and fails on any page error or CSP violation. Add `--screenshots <dir>` and `--color-scheme light|dark` to capture the UI.
