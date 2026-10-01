// The KAIRO Explorer integration boundary, exercised the way an external
// consumer would: build a model, call solver::solve(model, options, &report)
// once, serialize the outcome with the optimsolver.solve.v1 writer, and read
// the structured record back. No CLI, no terminal text, no MPS reader.
//
// This target links only solver_orchestrator and solve_report_json. What it
// proves:
//   * asking for a report does not change the SolveResult;
//   * the serialized record is a pure mapping of THAT SolveResult and THAT
//     SolveReport -- every stage time round-trips bit for bit, so the record
//     cannot come from a second execution;
//   * the writer recomputes nothing: given a deliberately inconsistent
//     synthetic report it echoes the report, not the model;
//   * early exits (presolve infeasibility, invalid model, refused CUDA) say
//     exactly which stages ran.

#include "json_report.h"
#include "pdlp/compute_backend.h"
#include "solver/orchestrator.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Minimal JSON reader. Numbers are read with strtod, so the writer's
// 17-significant-digit output round-trips to the identical double.
// ---------------------------------------------------------------------------

struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string text;
    std::vector<Json> items;
    std::map<std::string, Json> fields;

    bool isNull() const { return type == Type::Null; }
    const Json& operator[](const std::string& key) const {
        if (type != Type::Object) throw std::runtime_error("not an object at key " + key);
        auto it = fields.find(key);
        if (it == fields.end()) throw std::runtime_error("missing key " + key);
        return it->second;
    }
    bool has(const std::string& key) const { return type == Type::Object && fields.count(key) != 0; }
};

class JsonReader {
public:
    explicit JsonReader(const std::string& source) : s_(source) {}
    Json parse() {
        Json value = parseValue();
        skip();
        if (pos_ != s_.size()) throw std::runtime_error("trailing data");
        return value;
    }

private:
    const std::string& s_;
    std::size_t pos_ = 0;

    void skip() { while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) ++pos_; }
    void expect(char c) {
        skip();
        if (pos_ >= s_.size() || s_[pos_] != c) throw std::runtime_error(std::string("expected ") + c);
        ++pos_;
    }
    bool literal(const char* word) {
        const std::string w(word);
        if (s_.compare(pos_, w.size(), w) == 0) { pos_ += w.size(); return true; }
        return false;
    }
    std::string parseString() {
        expect('"');
        std::string out;
        while (pos_ < s_.size() && s_[pos_] != '"') {
            char c = s_[pos_++];
            if (c != '\\') { out += c; continue; }
            const char e = s_[pos_++];
            switch (e) {
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': out += static_cast<char>(std::stoi(s_.substr(pos_, 4), nullptr, 16)); pos_ += 4; break;
                default: out += e;
            }
        }
        expect('"');
        return out;
    }
    Json parseValue() {
        skip();
        Json v;
        if (pos_ >= s_.size()) throw std::runtime_error("unexpected end");
        const char c = s_[pos_];
        if (c == '{') {
            v.type = Json::Type::Object;
            ++pos_;
            skip();
            if (s_[pos_] == '}') { ++pos_; return v; }
            while (true) {
                skip();
                std::string key = parseString();
                expect(':');
                v.fields[key] = parseValue();
                skip();
                if (s_[pos_] == ',') { ++pos_; continue; }
                expect('}');
                return v;
            }
        }
        if (c == '[') {
            v.type = Json::Type::Array;
            ++pos_;
            skip();
            if (s_[pos_] == ']') { ++pos_; return v; }
            while (true) {
                v.items.push_back(parseValue());
                skip();
                if (s_[pos_] == ',') { ++pos_; continue; }
                expect(']');
                return v;
            }
        }
        if (c == '"') { v.type = Json::Type::String; v.text = parseString(); return v; }
        if (literal("null")) return v;
        if (literal("true")) { v.type = Json::Type::Bool; v.boolean = true; return v; }
        if (literal("false")) { v.type = Json::Type::Bool; v.boolean = false; return v; }
        char* end = nullptr;
        v.number = std::strtod(s_.c_str() + pos_, &end);
        if (end == s_.c_str() + pos_) throw std::runtime_error("bad token");
        v.type = Json::Type::Number;
        pos_ = static_cast<std::size_t>(end - s_.c_str());
        return v;
    }
};

// ---------------------------------------------------------------------------

const double INF = std::numeric_limits<double>::infinity();
int checks = 0;
int failures = 0;

void ck(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("    FAIL  %s\n", what.c_str());
    }
}

// A JSON number that must be exactly `expected`, or null when the report
// marks the value absent (NaN, or a negative "did not run" stage time).
bool sameNumber(const Json& value, double expected, bool absent) {
    if (absent) return value.isNull();
    return value.type == Json::Type::Number && value.number == expected;
}

bool sameEngine(const Json& value, solver::Engine engine) {
    return value.type == Json::Type::String && value.text == solver::toString(engine);
}

// Executed-engine fields use null for "nothing ran" (Engine::Unsupported).
bool sameExecuted(const Json& value, solver::Engine engine) {
    return engine == solver::Engine::Unsupported ? value.isNull() : sameEngine(value, engine);
}

struct Builder {
    model::Model m;
    void var(const char* name, model::VariableType type, double lo, double hi) {
        model::Variable v;
        v.name = name; v.type = type; v.lowerBound = lo; v.upperBound = hi;
        m.variables.push_back(v);
    }
    void row(const char* name, double lo, double hi, std::vector<model::LinearTerm> terms) {
        model::Constraint c;
        c.name = name; c.lowerBound = lo; c.upperBound = hi;
        c.linearTerms = std::move(terms);
        m.constraints.push_back(c);
    }
    void obj(model::ObjectiveSense sense, std::vector<model::LinearTerm> terms) {
        m.objective.sense = sense;
        m.objective.linearTerms = std::move(terms);
    }
};

using model::VariableType;

model::Model lpWithFixedColumn() {
    Builder b;
    b.var("x", VariableType::Continuous, 0, INF);
    b.var("y", VariableType::Continuous, 0, INF);
    b.var("fixed", VariableType::Continuous, 2, 2);
    b.row("labour",  -INF, 14.0, {{0, 2.0}, {1, 1.0}, {2, 1.0}});
    b.row("machine", -INF, 28.0, {{0, 4.0}, {1, 5.0}});
    b.row("material",-INF, 30.0, {{0, 2.0}, {1, 5.0}});
    b.obj(model::ObjectiveSense::Maximize, {{0, 3.0}, {1, 4.0}, {2, 1.0}});
    return b.m;
}

model::Model knapsack() {
    Builder b;
    b.var("a", VariableType::Binary, 0, 1);
    b.var("b", VariableType::Binary, 0, 1);
    b.var("c", VariableType::Binary, 0, 1);
    b.row("cap", -INF, 5.0, {{0, 2.0}, {1, 3.0}, {2, 4.0}});
    b.obj(model::ObjectiveSense::Maximize, {{0, 3.0}, {1, 4.0}, {2, 5.0}});
    return b.m;
}

model::Model convexQp() {
    Builder b;
    b.var("x", VariableType::Continuous, 0, INF);
    b.var("y", VariableType::Continuous, 0, INF);
    b.row("sum", -INF, 2.0, {{0, 1.0}, {1, 1.0}});
    b.obj(model::ObjectiveSense::Minimize, {{0, -2.0}, {1, -4.0}});
    b.m.objective.quadraticTerms = {{0, 0, 1.0}, {1, 1, 1.0}};
    return b.m;
}

model::Model presolveInfeasible() {
    Builder b;
    b.var("x", VariableType::Continuous, 0, 1);
    b.var("y", VariableType::Continuous, 0, 1);
    b.row("impossible", 5.0, INF, {{0, 1.0}});
    b.row("other", -INF, 1.0, {{0, 1.0}, {1, 1.0}});
    b.obj(model::ObjectiveSense::Minimize, {{0, 1.0}, {1, 1.0}});
    return b.m;
}

model::Model invalidBounds() {
    model::Model m = lpWithFixedColumn();
    m.variables[0].lowerBound = 5.0;
    m.variables[0].upperBound = 1.0;
    return m;
}

// One real execution, as an external consumer performs it.
struct Run {
    solver::SolveResult withoutReport;  // same call, no report requested
    solver::SolveResult result;
    solver::SolveReport report;
    Json record;
};

std::unique_ptr<Run> runAndSerialize(const model::Model& m, const solver::SolverOptions& options) {
    auto run = std::make_unique<Run>();
    run->withoutReport = solver::solve(m, options);
    run->result = solver::solve(m, options, &run->report);

    cli::JsonReportInput input;
    input.instancePath = m.name;
    input.originalVariables = m.variables.size();
    input.originalConstraints = m.constraints.size();
    input.originalModel = &m;
    input.report = &run->report;
    input.requestedBackend = solver::toString(options.backend);
    input.cudaDevice = options.cudaDevice;
    input.tolerance = options.tolerance;
    if (options.forceEngine) input.requestedEngine = solver::toString(*options.forceEngine);

    std::ostringstream text;
    ck(cli::writeJsonReport(text, input, run->result), "record writes");
    run->record = JsonReader(text.str()).parse();
    return run;
}

void checkValidation(const Json& value, const solver::ValidationSummary& check,
                     const std::string& label) {
    ck(value["passed"].boolean == check.passed, label + ": passed mirrors report");
    ck(value["status"].text == postsolve::toString(check.status), label + ": status mirrors report");
    ck(check.failure.empty() ? value["failure"].isNull() : value["failure"].text == check.failure,
       label + ": failure mirrors report");
    ck(sameNumber(value["max_bound_residual"], check.maxBoundResidual, std::isnan(check.maxBoundResidual)) &&
       sameNumber(value["max_constraint_residual"], check.maxConstraintResidual,
                  std::isnan(check.maxConstraintResidual)) &&
       sameNumber(value["objective"], check.objectiveValue, std::isnan(check.objectiveValue)),
       label + ": residuals and objective round-trip exactly");
}

// Every field the record derives from the run must equal the in-process value.
void checkMapping(const Run& run, const std::string& label) {
    const solver::SolveResult& r = run.result;
    const solver::SolveResult& plain = run.withoutReport;
    const solver::SolveReport& rep = run.report;
    const Json& j = run.record;

    // Requesting a report changed nothing about the outcome.
    ck(plain.status == r.status && plain.engine == r.engine && plain.engineReason == r.engineReason &&
       plain.executedEngine == r.executedEngine && plain.hasPrimal == r.hasPrimal &&
       plain.hasDuals == r.hasDuals && plain.integralityRespected == r.integralityRespected,
       label + ": SolveResult unchanged by the report");

    ck(j["schema"].text == "optimsolver.solve.v1", label + ": schema id");

    // RESULT
    ck(j["termination"]["status"].text == solver::toString(r.status), label + ": status");
    ck(j["termination"]["message"].text == r.message, label + ": message");
    ck(sameEngine(j["termination"]["dispatched_engine"], r.engine), label + ": dispatched engine");
    ck(sameExecuted(j["termination"]["executed_engine"], r.executedEngine), label + ": executed engine");
    ck(j["termination"]["engine_reason"].text == r.engineReason, label + ": engine reason");
    if (r.hasPrimal) {
        ck(sameNumber(j["objective"], r.objectiveValue, false), label + ": objective exact");
        bool primalSame = j["primal"].items.size() == r.variableValues.size();
        for (std::size_t i = 0; primalSame && i < r.variableValues.size(); ++i)
            primalSame = j["primal"].items[i].number == r.variableValues[i];
        ck(primalSame, label + ": primal exact");
        ck(j["self_reported"]["integrality_respected"].boolean == r.integralityRespected,
           label + ": integrality verdict is the pipeline's");
    } else {
        ck(j["objective"].isNull() && j["primal"].isNull() &&
           j["self_reported"]["integrality_respected"].isNull(), label + ": no point, no values");
    }
    ck(r.hasDuals ? j["duals"].items.size() == r.constraintDuals.size() : j["duals"].isNull(),
       label + ": duals presence");

    // MODEL
    if (rep.classification) {
        const auto& h = rep.classification->hints;
        const Json& c = j["classification"];
        ck(c["problem_class"].text == solver::toString(rep.classification->problemClass) &&
           c["num_rows"].number == static_cast<double>(h.numRows) &&
           c["num_columns"].number == static_cast<double>(h.numColumns) &&
           c["nonzeros"].number == static_cast<double>(h.numNonzeros) &&
           c["num_binary"].number == h.numBinary && c["num_integer"].number == h.numInteger &&
           c["num_continuous"].number == h.numContinuous &&
           c["has_big_m"].boolean == h.hasBigM && c["symmetric_groups"].number == h.symmetricGroups,
           label + ": classification is the report's");
    } else {
        ck(j["classification"].isNull(), label + ": no classification recorded");
    }

    // PRESOLVE
    if (rep.presolve) {
        const auto& p = *rep.presolve;
        const Json& q = j["presolve"];
        ck(q["infeasible"].boolean == p.infeasible && q["converged"].boolean == p.converged &&
           q["original_variables"].number == static_cast<double>(p.originalVariables) &&
           q["reduced_variables"].number == static_cast<double>(p.reducedVariables) &&
           q["original_constraints"].number == static_cast<double>(p.originalConstraints) &&
           q["reduced_constraints"].number == static_cast<double>(p.reducedConstraints) &&
           q["original_nonzeros"].number == static_cast<double>(p.originalNonzeros) &&
           q["reduced_nonzeros"].number == static_cast<double>(p.reducedNonzeros) &&
           q["transformations"].number == static_cast<double>(p.transformationCount) &&
           q["transformations_by_type"]["fix_variable"].number ==
               static_cast<double>(p.transformationsByType.fixVariable),
           label + ": presolve summary is the report's");
    } else {
        ck(j["presolve"].isNull(), label + ": no presolve recorded");
    }

    // DISPATCH
    const Json& d = j["dispatch"];
    ck(d["invoked"].boolean == rep.dispatch.dispatcherInvoked, label + ": dispatcher invoked flag");
    ck(sameEngine(d["engine"], rep.dispatch.engine) && rep.dispatch.engine == r.engine,
       label + ": dispatch engine");
    ck(sameExecuted(d["executed_engine"], rep.dispatch.executedEngine) &&
       rep.dispatch.executedEngine == r.executedEngine, label + ": dispatch executed engine");
    ck(rep.dispatch.reason.empty() ? d["reason"].isNull() : d["reason"].text == rep.dispatch.reason,
       label + ": dispatch reason");

    // EXECUTION: backend is SolveResult's; stage times are the report's,
    // bit for bit -- a second execution could not reproduce them.
    const bool engineRan = r.executedEngine != solver::Engine::Unsupported &&
                           r.status != solver::SolveStatus::InvalidModel;
    ck(engineRan ? j["compute_backend"]["executed"].text == solver::toString(r.executedBackend)
                 : j["compute_backend"]["executed"].isNull(), label + ": executed backend");
    const auto& t = rep.stageSeconds;
    const Json& st = j["stage_seconds"];
    ck(sameNumber(st["validation"], t.validation, t.validation < 0) &&
       sameNumber(st["classification"], t.classification, t.classification < 0) &&
       sameNumber(st["presolve"], t.presolve, t.presolve < 0) &&
       sameNumber(st["dispatch"], t.dispatch, t.dispatch < 0) &&
       sameNumber(st["engine"], t.engine, t.engine < 0) &&
       sameNumber(st["reduced_validation"], t.reducedValidation, t.reducedValidation < 0) &&
       sameNumber(st["postsolve"], t.postsolve, t.postsolve < 0),
       label + ": every stage time round-trips exactly");
    ck(t.total == r.solveSeconds && sameNumber(st["total"], r.solveSeconds, false) &&
       sameNumber(j["work"]["solve_seconds"], r.solveSeconds, false),
       label + ": total is this execution's solveSeconds");

    // VALIDATION
    if (rep.reducedValidation) {
        checkValidation(j["validation"]["reduced_space"], *rep.reducedValidation, label + " reduced");
        ck(sameNumber(j["validation"]["reduced_space"]["engine_reported_objective"],
                      rep.reducedValidation->engineReportedObjective,
                      std::isnan(rep.reducedValidation->engineReportedObjective)),
           label + ": engine-reported objective");
    } else {
        ck(j["validation"]["reduced_space"].isNull(), label + ": no reduced validation recorded");
    }
    if (rep.postsolve) {
        checkValidation(j["validation"]["original_space"], *rep.postsolve, label + " original");
    } else {
        ck(j["validation"]["original_space"].isNull(), label + ": no postsolve recorded");
    }
}

void testLp() {
    const model::Model m = lpWithFixedColumn();
    const auto run = runAndSerialize(m, {});
    checkMapping(*run, "LP");
    ck(run->result.status == solver::SolveStatus::Optimal &&
       run->result.executedEngine == solver::Engine::DualSimplex, "LP: optimal on dual simplex");
    ck(run->record["presolve"]["reduced_variables"].number <
       run->record["presolve"]["original_variables"].number, "LP: presolve reduced the model");
    ck(run->record["instance"]["objective_sense"].text == "max", "LP: objective sense");
}

void testMilp() {
    const model::Model m = knapsack();
    const auto run = runAndSerialize(m, {});
    checkMapping(*run, "MILP");
    ck(run->result.executedEngine == solver::Engine::BranchAndCut, "MILP: branch-and-cut");
    ck(run->record["classification"]["num_binary"].number == 3, "MILP: binaries");
    ck(run->record["self_reported"]["integrality_respected"].boolean, "MILP: integral answer");
    ck(std::abs(run->record["objective"].number - 7.0) < 1e-9, "MILP: optimum 7");
}

void testQp() {
    const model::Model m = convexQp();
    const auto run = runAndSerialize(m, {});
    checkMapping(*run, "QP");
    ck(run->result.executedEngine == solver::Engine::Qp, "QP: qp engine");
    ck(run->record["classification"]["problem_class"].text == "QP", "QP: classified QP");
}

void testPresolveInfeasible() {
    const model::Model m = presolveInfeasible();
    const auto run = runAndSerialize(m, {});
    checkMapping(*run, "infeasible");
    const Json& j = run->record;
    ck(j["termination"]["status"].text == "infeasible", "infeasible: status");
    ck(!j["dispatch"]["invoked"].boolean, "infeasible: dispatcher never ran");
    ck(j["presolve"]["infeasible"].boolean, "infeasible: presolve proved it");
    ck(j["stage_seconds"]["dispatch"].isNull() && j["stage_seconds"]["engine"].isNull() &&
       j["stage_seconds"]["postsolve"].isNull(), "infeasible: later stages null");
    ck(j["compute_backend"]["executed"].isNull(), "infeasible: no backend ran");
}

void testInvalidModel() {
    const model::Model m = invalidBounds();
    const auto run = runAndSerialize(m, {});
    checkMapping(*run, "invalid");
    const Json& j = run->record;
    ck(j["termination"]["status"].text == "invalid_model", "invalid: status");
    ck(j["classification"].isNull() && j["presolve"].isNull(), "invalid: nothing after validation");
    ck(!j["dispatch"]["invoked"].boolean && j["dispatch"]["executed_engine"].isNull(),
       "invalid: no dispatch");
    ck(j["stage_seconds"]["validation"].type == Json::Type::Number &&
       j["stage_seconds"]["classification"].isNull(), "invalid: only validation timed");
}

void testForcedEngine() {
    const model::Model m = lpWithFixedColumn();
    solver::SolverOptions options;
    options.forceEngine = solver::Engine::Pdlp;
    const auto run = runAndSerialize(m, options);
    checkMapping(*run, "forced");
    ck(run->record["dispatch"]["reason"].text == "engine forced by the caller", "forced: reason");
    ck(run->record["settings"]["requested_engine"].text == "pdlp", "forced: request echoed");
}

void testBackend() {
    const model::Model m = lpWithFixedColumn();
    solver::SolverOptions cpu;
    cpu.forceEngine = solver::Engine::Pdlp;
    cpu.backend = solver::ComputeBackend::Cpu;
    const auto run = runAndSerialize(m, cpu);
    checkMapping(*run, "backend cpu");
    ck(run->record["compute_backend"]["requested"].text == "cpu" &&
       run->record["compute_backend"]["executed"].text == "cpu", "backend cpu: ran on the CPU");
    ck(run->record["compute_backend"]["reason"].text == run->result.backendReason,
       "backend cpu: the engine's own reason");

    solver::SolverOptions cuda = cpu;
    cuda.backend = solver::ComputeBackend::Cuda;
    const auto gpu = runAndSerialize(m, cuda);
    checkMapping(*gpu, "backend cuda");
    if (!pdlp::cudaAvailability(0).usable) {
        // Refused before PDLP ran: the dispatcher chose it, nothing executed.
        ck(gpu->result.status == solver::SolveStatus::Unsupported, "backend cuda: refused");
        ck(gpu->record["dispatch"]["invoked"].boolean &&
           gpu->record["dispatch"]["engine"].text == "pdlp" &&
           gpu->record["dispatch"]["executed_engine"].isNull(), "backend cuda: chosen, not executed");
        ck(gpu->record["compute_backend"]["executed"].isNull(), "backend cuda: no silent CPU run");
        ck(gpu->record["validation"]["reduced_space"].isNull(), "backend cuda: nothing validated");
    } else {
        ck(gpu->record["compute_backend"]["executed"].text == "cuda", "backend cuda: ran on the GPU");
    }
}

// The writer must never recompute: fed a report that contradicts the model,
// it has to echo the report. (A writer that re-classified or re-presolved
// would print the model's real values here.)
void testWriterOnlyMapsTheReport() {
    const model::Model m = lpWithFixedColumn();  // really an LP with 3 columns
    solver::SolveResult result;
    result.status = solver::SolveStatus::LimitReached;
    result.engine = solver::Engine::Miqp;
    result.engineReason = "synthetic reason";
    result.executedEngine = solver::Engine::Miqp;
    result.solveSeconds = 9.25;

    solver::SolveReport report;
    solver::Classification fake;
    fake.problemClass = solver::ProblemClass::MIQP;
    fake.hints.numBinary = 42;
    fake.hints.numRows = 1000;
    fake.hints.numNonzeros = 31337;
    report.classification = fake;
    solver::PresolveSummary presolve;
    presolve.originalVariables = 777;
    presolve.reducedVariables = 7;
    presolve.reducedNonzeros = 5;
    presolve.transformationCount = 3;
    presolve.transformationsByType.substituteVariable = 3;
    report.presolve = presolve;
    report.dispatch.dispatcherInvoked = true;
    report.dispatch.engine = solver::Engine::Miqp;
    report.dispatch.reason = "synthetic reason";
    report.dispatch.executedEngine = solver::Engine::Miqp;
    report.stageSeconds.presolve = 1.5;
    report.stageSeconds.engine = 2.25;
    report.stageSeconds.total = 9.25;

    cli::JsonReportInput input;
    input.originalModel = &m;
    input.report = &report;
    std::ostringstream text;
    cli::writeJsonReport(text, input, result);
    const Json j = JsonReader(text.str()).parse();

    ck(j["classification"]["problem_class"].text == "MIQP" &&
       j["classification"]["num_binary"].number == 42 &&
       j["classification"]["nonzeros"].number == 31337, "writer: classification echoed, not recomputed");
    ck(j["presolve"]["original_variables"].number == 777 && j["presolve"]["reduced_variables"].number == 7 &&
       j["presolve"]["transformations_by_type"]["substitute_variable"].number == 3,
       "writer: presolve echoed, not rerun");
    ck(j["dispatch"]["engine"].text == "miqp" && j["dispatch"]["reason"].text == "synthetic reason",
       "writer: dispatch echoed, not recomputed");
    ck(j["validation"]["reduced_space"].isNull() && j["validation"]["original_space"].isNull(),
       "writer: absent validation stays absent");
    ck(j["stage_seconds"]["presolve"].number == 1.5 && j["stage_seconds"]["engine"].number == 2.25 &&
       j["stage_seconds"]["dispatch"].isNull() && j["stage_seconds"]["total"].number == 9.25,
       "writer: stage times echoed");

    // A record without a report (solve never called) has no stage sections.
    cli::JsonReportInput bare;
    std::ostringstream bareText;
    cli::writeJsonReport(bareText, bare, result);
    const Json b = JsonReader(bareText.str()).parse();
    ck(b["classification"].isNull() && b["presolve"].isNull() && b["dispatch"].isNull() &&
       b["validation"].isNull() && b["stage_seconds"]["total"].isNull(),
       "writer: no report, no stage sections");
}

}  // namespace

int main() {
    try {
        testLp();
        testMilp();
        testQp();
        testPresolveInfeasible();
        testInvalidModel();
        testForcedEngine();
        testBackend();
        testWriterOnlyMapsTheReport();
    } catch (const std::exception& error) {
        std::printf("    FAIL  exception: %s\n", error.what());
        return 1;
    }
    std::printf("test_solve_contract: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
