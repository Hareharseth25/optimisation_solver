#include "benchmark/benchmarkresult.h"

// This file is intentionally minimal.
//
// BenchmarkResult.h is implemented as a header-only file
// (all functions are declared `inline`), so there is no
// additional logic to compile here.
//
// This .cpp exists to:
//   1. Keep the folder structure consistent with
//      include/model + src/model, include/mps + src/mps,
//      include/presolve + src/presolve.
//   2. Act as a placeholder for future non-template,
//      non-inline code in the benchmark layer (e.g. once
//      solver-specific result parsers are added here).
//   3. Provide a single compile unit that verifies
//      BenchmarkResult.h compiles cleanly on its own.