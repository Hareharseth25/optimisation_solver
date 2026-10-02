# KAIRO Desktop

Native, offline desktop application for KAIRO — Kernel for Advanced Integer & Real Optimization. Built with **Qt 6 Widgets + C++17** and **CMake**, for macOS, Windows and Linux. It runs the solver in-process. It needs **no browser, web server, localhost port, network, Python or JavaScript**, and has no account or telemetry.

```text
                     KAIRO
                       │
              ┌────────┴────────┐
         KAIRO Core          Interfaces
              │            ┌────┴─────┐
        engines, solve()  Desktop     CLI
                          (Qt 6)   (optimsolver)
                           │
                 ┌─────────┼─────────┐
               macOS    Windows    Linux
```

## How it talks to KAIRO Core

`src/core/KairoSession` is the only code that touches the solver. It calls it **in-process**:

- `openModel()` uses `mps::MpsReader`, `model::Model::validate()` and `solver::countNonzeros()`.
- `solve()` runs the same per-run steps as `optimsolver solve --json`:
  1. read the MPS file;
  2. run the structural check;
  3. `solver::solve(model, options, &report)`;
  4. KAIRO's own record writer (`cli::writeJsonReport`, the `solve_report_json` library) turns that call's `SolveResult` and `SolveReport` into the `optimsolver.solve.v1` record.

The GUI interprets only that record (`src/record/RecordReaders`), so a live run and a saved run use exactly the same readers. The readers are tested against real solves (`tests/test_desktop.cpp`). Their output on 21 real records is also pinned in `tests/fixtures/records/expected.tsv`. Those records come from the retired web prototype, and were checked against its readers with zero differences before it was removed. The GUI never classifies, presolves, dispatches, validates or parses CLI text.

Trust wording: "Optimal", "Optimal — according to the solver", "Optimal for the continuous relaxation", "Global optimality evidence not independently recorded", *Checked / Not checked / Not available / Reported by engine / Proved by presolve*. It never says "certified", "guaranteed" or "proven optimal".

## Build and run

Needs CMake ≥ 3.20, a C++17 compiler and **Qt 6.5 or newer** (Core, Gui, Widgets, Concurrent; Test for the tests). The top-level build adds the app automatically when Qt 6 is found; `-DKAIRO_BUILD_DESKTOP=OFF` skips it. Without Qt, the solver and CLI build exactly as before. Release builds should use the official Qt binaries (Qt online installer, or `aqt install-qt`). Homebrew's Qt is fine for development but drags extra Homebrew-built libraries into a deployed bundle.

```sh
# macOS (official Qt, e.g. ~/Qt/6.8.3/macos)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=$HOME/Qt/6.8.3/macos
cmake --build build --target KAIRO -j
open build/desktop/KAIRO.app

# Linux x64 (official Qt, e.g. ~/Qt/6.8.3/gcc_64)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=$HOME/Qt/6.8.3/gcc_64
cmake --build build --target KAIRO
./build/desktop/KAIRO

# Windows x64 (Developer PowerShell for VS 2022; official Qt msvc2022_64)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64
cmake --build build --target KAIRO
build\desktop\KAIRO.exe          # Qt's bin directory must be on PATH until the app is deployed
```

Tests: `cmake --build build --target kairo_desktop_tests KAIRO optimsolver && ctest --test-dir build -R desktop_ --output-on-failure`.

**macOS SDK note.** Qt 6.8.3's CMake files link the AGL framework, which the macOS 26 SDK no longer ships (`ld: framework 'AGL' not found`). With Xcode 26, either build against a macOS 15 SDK (`-DCMAKE_OSX_SYSROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk`, as the release build below does) or use a newer Qt.

Verification modes (used by CTest; headless with `-platform offscreen`):

```sh
KAIRO --capture MODEL.mps --out shot.png [--engine pdlp] [--backend cpu] [--time-limit 5] [--threads 1] [--save] [--export run.json]
KAIRO --import run.json --out shot.png                         # render an imported record (never solves)
KAIRO --compare a.json --compare b.json --out shot.png         # render a comparison and print its rows
```

`--import`/`--compare` use a temporary run directory unless `--runs-dir` is given, so they never touch your own saved runs.

## Workflow

1. **Open Model…** (Ctrl/⌘+O) or drop an `.mps`/`.qps` file on the window. KAIRO's reader shows the name, dimensions, nonzeros, objective sense and the structural check. Classification is shown only after the solve, because it is a solve stage.
2. **Solver options.** These map onto `solver::SolverOptions`:
   - engine (automatic, or a `solver::Engine`);
   - backend (auto / CPU / CUDA, only for engines with a CUDA backend);
   - time limit;
   - threads;
   - CUDA device.
3. **Solve** runs on a worker thread. The window shows only "Running KAIRO…": there are no progress events.
4. **The analysis** shows Result, Evidence, Pipeline (click a stage to jump to its details), Model analysis, Presolve impact, Dispatch decision, Execution and Validation details.
5. **Save run** (Ctrl/⌘+S) stores the run on this computer.
6. **Saved runs:** open (Enter or double-click), delete (Delete/Backspace; more than one asks first), **Clear all…** (always asks), **Show current run** (Ctrl/⌘+0) to return to the live run. Imported runs are listed with an `[Imported]` tag. In the Result section, an imported run says "imported record · added … — shown as recorded; not re-solved on this computer".
7. **Compare:** select two saved runs (Shift/Ctrl/⌘-click) → **Compare** (Ctrl/⌘+Shift+C). See below.
8. **Export Run…** (Ctrl/⌘+E) writes the shown run's `optimsolver.solve.v1` record. **Import Run…** (Ctrl/⌘+I) reads one.

Menus: **File** (Open Model, Open Recent, Import Run, Export Run, Quit), **Run** (Solve Ctrl/⌘+R, Save Run, Show Current Run, Compare Selected Runs), **View** (Go to Result … Validation Details, Ctrl/⌘+1…8; Saved Runs List Ctrl/⌘+L), **Help** (About). Open Recent keeps up to 8 model **paths** in the platform settings store (`QSettings`), never model contents. Failures to save, import, export or open a recent file are shown in a native message box with the reason. The theme follows the system light/dark setting, including a change while the app is running.

**Running state and cancellation.** While a solve runs, the report shows only "Running KAIRO… *file*" and Solve is disabled. KAIRO Core has no cancellation API: `solver::solve()` runs to completion or to its time limit. Cancel is therefore not offered; use the time limit to bound a run. Quitting during a solve waits for it to finish.

## Comparing runs

`src/record/Comparison` (Qt Core) builds the comparison from the two records through the same readers; `src/ui/ComparisonView` only lays it out.

- **Same model** means the same `instance.sha256` recorded by KAIRO. Nothing else (file name, dimensions) counts.
- Groups: Identity (file, SHA-256, class, dimensions, build commit/type, origin, saved), Request (engine, backend, CUDA device, time limit, threads, tolerance), Outcome (status, result wording, selected vs executed engine, backend, solution point; for the same model also objective, integrality, original-space validation, max constraint violation, KKT residual), Work and timing (total, dispatch, presolve, engine, postsolve, both validations, iterations, nodes), Presolve (reduced dimensions, transformations; same model only).
- **Different hashes:** "Different model inputs — direct comparison is limited." Objective, validation/residual and presolve rows read "Not compared — different model inputs".
- A value a record does not hold reads **Not recorded**, never 0.
- Differences are stated, never judged: `B − A = +1.02 ms`, or "differs". Run A is the earlier saved run. A difference is stated only when the displayed values differ. No "better", "best", "winner" or ranking.
- The engine stage time is labelled "(execution)" or "(engine path)" (nothing executed); the two kinds are never subtracted.
- Notes say when builds differ (the commit is recorded at configure time) and that times are single measurements on an unrecorded host.

## Export and import

- **Export** writes the record alone, indented and atomically (`QSaveFile`). It is exactly the `optimsolver.solve.v1` document, the same contract `optimsolver solve --json` writes, so CLI records can be imported too.
- **Import** reads a `.json` file (at most 512 MiB). It accepts an `optimsolver.solve.v1` record, or a `kairo.desktop.saved_run.v1` file, whose record it takes. The shape is checked before anything is stored: schema, required objects, nullable objects, KAIRO status, `primal`/`duals`/`reduced_costs` as number arrays or null, and objective as a number or null. Anything else is refused with the reasons.
- Import **never runs the solver** or anything else; the record is data. It is stored marked `source: "imported"` and rendered by the same readers as any other run, so its wording follows the recorded evidence exactly. Only the model's file name is kept from `instance.path` (a CLI record may hold a full path from another machine).

## Saved runs

Saved runs are one JSON file per run in the per-user application data directory (`QStandardPaths::AppLocalDataLocation`/`runs`, overridable with `--runs-dir`). Each file holds format `kairo.desktop.saved_run.v1`: a local UUID, the time saved, the file name and size (`null` for imports), `source` (`"live"` or `"imported"`; files written before `source` existed read as live), and the **unchanged** `optimsolver.solve.v1` record. Model files are never copied. Files are written atomically (`QSaveFile`), and "Clear all" deletes only files this store wrote.

## Layout

| path | role |
|---|---|
| `src/core/KairoSession.*` | in-process bridge to KAIRO Core |
| `src/record/RecordReaders.*`, `Format.*` | record → presentation state (Qt Core only) |
| `src/record/Comparison.*` | two records → comparison rows and notes (Qt Core only) |
| `src/history/RunStore.*` | local saved runs, record shape check, export/import |
| `src/ui/MainWindow.*` | window, menus, model input, options, solve thread, saved runs, import/export |
| `src/ui/ReportView.*` | the analysis sections |
| `src/ui/ComparisonView.*` | two runs side by side |
| `src/ui/Theme.*` | light/dark colours from the platform palette |
| `tests/test_desktop.cpp` | QtTest: readers, comparison, import/export, history and window flow against real KAIRO solves |
| `tests/export_runs.cmake` | CTest fixture: solve + export through the real app |
| `tests/fixtures/records/` | 21 real `optimsolver.solve.v1` records and the expected reading of each |
| `packaging/linux/org.kairo.desktop.desktop` | freedesktop entry (installed on Linux) |
| `packaging/macos/finalize_bundle.cmake` | strips build-machine paths from a deployed bundle, ad hoc signs it |
| `packaging/macos/KAIRO.icns`, `packaging/windows/KAIRO.{ico,rc}`, `packaging/icons/` | application icons |
| `packaging/linux/make_appimage.sh` | AppImage from a deployed Linux install tree |

## Packaging and release artifacts

With `-DKAIRO_DESKTOP_DEPLOY=ON`, `cmake --install` runs Qt's own deployment (`qt_generate_deploy_app_script`; on Windows `qt_deploy_runtime_dependencies` with the plugin directory set to `bin`):
- **macOS:** `macdeployqt`, then `packaging/macos/finalize_bundle.cmake`. That step strips build-machine rpaths and install names and ad hoc signs the bundle.
- **Windows:** `windeployqt` with the Qt plugins next to `KAIRO.exe` (`bin/platforms/qwindows.dll`, `bin/styles/`, …), plus the MSVC runtime DLLs via CMake's `InstallRequiredSystemLibraries`. Everything is in `bin/`, so `bin/` alone is the application.
- **Linux:** Qt's runtime-dependency deployment into `bin/`, `lib/` and `plugins/`, plus the freedesktop entry and icon.

KAIRO Core is linked statically into the executable, so there are no solver libraries to ship separately.

**macOS → universal `KAIRO.app` and DMG (macOS 12+):**
```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DKAIRO_DESKTOP_DEPLOY=ON \
  -DCMAKE_PREFIX_PATH=$HOME/Qt/6.8.3/macos -DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64" \
  -DCMAKE_OSX_SYSROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk   # only with Xcode 26 + Qt 6.8
cmake --build build-release -j
cmake --install build-release --prefix dist
hdiutil create -volname KAIRO -srcfolder dist/KAIRO.app -ov -format UDZO KAIRO-0.1.0-macos-universal.dmg
```
The bundle is **ad hoc signed only**. To distribute it outside your own machines (the step not done here; it needs an Apple Developer ID):
```sh
codesign --force --deep --options runtime --timestamp --sign "Developer ID Application: <NAME> (<TEAMID>)" dist/KAIRO.app
hdiutil create -volname KAIRO -srcfolder dist/KAIRO.app -ov -format UDZO KAIRO-0.1.0-macos-universal.dmg
codesign --sign "Developer ID Application: <NAME> (<TEAMID>)" --timestamp KAIRO-0.1.0-macos-universal.dmg
xcrun notarytool submit KAIRO-0.1.0-macos-universal.dmg --keychain-profile <profile> --wait
xcrun stapler staple KAIRO-0.1.0-macos-universal.dmg
```

**Windows x64 → folder / zip** (Developer PowerShell for VS 2022):
```powershell
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DKAIRO_DESKTOP_DEPLOY=ON -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64
cmake --build build-release
cmake --install build-release --prefix dist
Compress-Archive -Path dist\bin -DestinationPath KAIRO-0.1.0-windows-x64.zip   # dist\bin\KAIRO.exe + Qt DLLs and plugins + MSVC runtime
```
Code signing (`signtool sign /fd sha256 /tr <timestamp-url> /td sha256 /f <cert> dist\bin\KAIRO.exe`) needs a certificate and is not done.

**Linux x64 → AppImage:**
```sh
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DKAIRO_DESKTOP_DEPLOY=ON -DCMAKE_PREFIX_PATH=$HOME/Qt/6.8.3/gcc_64
cmake --build build-release
cmake --install build-release --prefix dist
desktop/packaging/linux/make_appimage.sh dist          # → KAIRO-0.1.0-x86_64.AppImage (downloads appimagetool)
```
The AppImage carries Qt and its plugins. The X11/xcb system libraries (e.g. `libxcb-cursor0`) and OpenGL come from the host, as is usual for AppImages.

`.github/workflows/desktop.yml` runs all three recipes, plus the tests and a launch of each installed app, on GitHub-hosted runners with official Qt 6.8. It uploads the DMG, the zip and the AppImage.

## Supported targets and status

**Tested** (built and run in this repository's development environment):

| Target | Evidence |
|---|---|
| macOS arm64 (Apple Silicon, macOS 26) | Clean builds with official Qt 6.8.3 (universal) and Homebrew Qt 6.11.2, 0 warnings. Full CTest passes. The desktop suite passes 31/31. The 11 real scenarios ran in the deployed app. The deployed `KAIRO.app` was copied out of the source tree and run with an empty environment: every non-system library loaded from inside the bundle, with no `/opt/homebrew` paths. The DMG was mounted and the app solved from it. The GUI was launched through LaunchServices. |
| macOS x86_64 slice | The same universal build, run under Rosetta 2: the desktop suite passes 31/31, and a real solve ran through the deployed bundle. Not run on Intel hardware. |

**Configured but not tested.** No Windows or Linux machine, VM or container was available, and the CI workflow has not run yet (running it requires pushing a commit):

| Target | State |
|---|---|
| Windows x64 (MSVC 2022) | CMake, `windeployqt` deployment, the MSVC runtime, an icon resource and a CI job (build, test, deploy, launch, zip). **Never built.** |
| Linux x64 (GCC) | CMake, Qt deployment, desktop entry, icon, `RPATH $ORIGIN/../lib`, the AppImage script and a CI job (build, test, deploy, launch under Xvfb, AppImage). **Never built on Linux.** As a partial check, the whole project (core, CLI, all desktop sources) compiled with GCC 16 / libstdc++ on macOS, and the core tests passed 78/78 there. The desktop link step failed only because of the GCC-vs-libc++ Qt mismatch on macOS. |

**Not supported:**
- **Windows ARM64.** Core code (`pdlp_engine/src/parallel.cpp`, `qp_engine/src/parallel.cpp`) uses GCC-style inline assembly under `_M_ARM64`, which MSVC rejects. It was deliberately left unchanged in this release.
- **Linux ARM64.** Not configured or validated.

## Current status (2026-10-02, after PR #20 was merged)

*This section supersedes the platform status above, which was written before CI had run and is kept as the pre-merge record.*

PR #20 (`release/kairo-v1`) is merged into `main`. The `desktop` workflow (`.github/workflows/desktop.yml`) passed for the release commit `bcd132f`, on both the push run and the pull-request run:

| Runner | Result | What ran |
|---|---|---|
| `ubuntu-24.04` (Linux x64, GCC) | passed | build (official Qt 6.8) → desktop CTest cases → install with Qt deployment → installed app launched under Xvfb with the native platform plugin → AppImage built and launched → artifact `kairo-desktop-Linux` |
| `windows-2022` (Windows x64, MSVC) | passed | build → desktop CTest cases → install with `windeployqt` and the MSVC runtime → installed `KAIRO.exe` launched with the native platform plugin → zip → artifact `kairo-desktop-Windows` |
| `macos-14` | passed | build → desktop CTest cases → install with `macdeployqt` → bundle checked for build-machine library paths, signature verified → installed app launched with the native platform plugin → DMG → artifact `kairo-desktop-macOS` |

In each launch step the installed application solved `tests/cli/presolve_reduction.mps` through `--capture`. The step required the rendered pipeline to finish at `result:completed`.

The macOS development and release validation is unchanged: it was done locally, as documented above.

**Supported targets:**
- **Supported and checked in CI:** macOS, Windows x64, Linux x64.
- **Not supported:** Windows ARM64 (reason above).
- **Not validated, not supported:** Linux ARM64.

**Still open:**
- macOS builds are **ad hoc signed** only. Developer ID signing and notarization are not done (commands under "Packaging and release artifacts").
- Windows code signing is not done.
- **No cancellation.** KAIRO Core has no cancellation API, so the desktop offers none; the time limit bounds a solve.
- CI checks build, tests, deployment and a real solve through each installed app. It does not exercise the interactive GUI by hand, and it is not a performance or scale validation.
