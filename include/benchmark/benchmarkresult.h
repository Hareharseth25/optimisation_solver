#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <sstream>

namespace benchmark {

// ============================================================
// SuccessStatus
// ============================================================

enum class SuccessStatus {
    Success,       // exited 0, didn't time out, didn't fail to launch
    NonZeroExit,   // ran to completion but returned a non-zero code
    TimedOut,
    LaunchFailed,
    Unknown
};

inline std::string toString(SuccessStatus status) {

    switch (status) {

        case SuccessStatus::Success:      return "SUCCESS";
        case SuccessStatus::NonZeroExit:  return "NON_ZERO_EXIT";
        case SuccessStatus::TimedOut:     return "TIMED_OUT";
        case SuccessStatus::LaunchFailed: return "LAUNCH_FAILED";
        default:                          return "UNKNOWN";
    }
}

// ============================================================
// SolverStatistics
//
// Extension point for solver-specific parsing. Everything is
// optional and unset for now — no parsing happens in this layer.
// A later parser fills these in after inspecting stdout/stderr.
// ============================================================

struct SolverStatistics {

    std::optional<double> objectiveValue;

    std::optional<std::string> primalStatus;
    std::optional<std::string> dualStatus;

    std::optional<long long> iterations;
    std::optional<long long> peakMemoryKB;

    // presolve statistics
    std::optional<long long> presolveRowsRemoved;
    std::optional<long long> presolveColsRemoved;
    std::optional<double> presolveTimeMs;
};

// ============================================================
// BenchmarkResult
// ============================================================

struct BenchmarkResult {

    // identification
    std::string solverName;
    std::string instanceName;

    // raw process-level info
    int exitCode = -1;
    bool timedOut = false;
    bool launchFailed = false;
    std::chrono::milliseconds runtime{0};

    std::string stdoutOutput;
    std::string stderrOutput;

    // derived status
    SuccessStatus status = SuccessStatus::Unknown;

    // extension point — empty until solver-specific parsers exist
    SolverStatistics stats;

    bool isSuccess() const {
        return status == SuccessStatus::Success;
    }
};

// ============================================================
// buildBenchmarkResult
//
// Converts a ProcessResult-shaped object into a BenchmarkResult.
// This is a template on purpose: it only needs an object that has
// .exitCode, .timedOut, .launchFailed, .runtime, .stdoutOutput,
// .stderrOutput — the exact shape of benchmark_model's
// ProcessResult — without this header having to #include that
// file. If a proper ProcessManager header is split out later,
// this still just works.
// ============================================================

template <typename ProcessResultT>
inline BenchmarkResult buildBenchmarkResult(
    const std::string& solverName,
    const std::string& instanceName,
    const ProcessResultT& processResult
) {
    BenchmarkResult result;

    result.solverName = solverName;
    result.instanceName = instanceName;

    result.exitCode = processResult.exitCode;
    result.timedOut = processResult.timedOut;
    result.launchFailed = processResult.launchFailed;
    result.runtime = processResult.runtime;

    result.stdoutOutput = processResult.stdoutOutput;
    result.stderrOutput = processResult.stderrOutput;

    if (processResult.launchFailed) {
        result.status = SuccessStatus::LaunchFailed;
    } else if (processResult.timedOut) {
        result.status = SuccessStatus::TimedOut;
    } else if (processResult.exitCode == 0) {
        result.status = SuccessStatus::Success;
    } else {
        result.status = SuccessStatus::NonZeroExit;
    }

    return result;
}

// ============================================================
// summarize
//
// Short human-readable summary line, e.g. for console/log output.
// ============================================================

inline std::string summarize(const BenchmarkResult& result) {

    std::ostringstream out;

    out << result.solverName << " | " << result.instanceName
        << " | status=" << toString(result.status)
        << " | exitCode=" << result.exitCode
        << " | runtime_ms=" << result.runtime.count();

    return out.str();
}

} // namespace benchmark