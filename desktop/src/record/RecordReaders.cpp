#include "RecordReaders.h"

#include "Format.h"

#include <QJsonArray>
#include <QHash>
#include <QJsonValue>

#include <tuple>
#include <utility>

namespace kairo::record {
namespace {

namespace f = kairo::format;

bool absent(const QJsonValue& value) { return value.isNull() || value.isUndefined(); }
Number num(const QJsonValue& value) { return value.isDouble() ? Number(value.toDouble()) : std::nullopt; }
QString str(const QJsonValue& value) { return value.isString() ? value.toString() : QString(); }
QJsonObject obj(const QJsonValue& value) { return value.isObject() ? value.toObject() : QJsonObject(); }
bool passed(const QJsonObject& check) { return check.value("passed").toBool(false); }

QString plural(Number n, const QString& one, const QString& many) {
    return f::count(n) + QLatin1Char(' ') + (n && *n == 1 ? one : many);
}

}  // namespace

// ============================================================ dispatch

const PseudoEngine* pseudoEngine(const QString& engine) {
    static const PseudoEngine infeasible{QStringLiteral("infeasible"), QStringLiteral("Early exit"),
                                         QStringLiteral("Presolve settled the solve; no engine is needed.")};
    static const PseudoEngine trivial{QStringLiteral("trivial path"), QStringLiteral("Trivial path"),
                                      QStringLiteral("The solution is read from variable bounds; no iterative engine runs.")};
    static const PseudoEngine unsupported{QStringLiteral("no suitable engine"), QStringLiteral("No suitable engine"),
                                          QStringLiteral("No KAIRO engine can solve the model as posed.")};
    if (engine == QLatin1String("infeasible")) return &infeasible;
    if (engine == QLatin1String("trivial")) return &trivial;
    if (engine == QLatin1String("unsupported")) return &unsupported;
    return nullptr;
}

bool isPseudoEngine(const QString& engine) { return pseudoEngine(engine) != nullptr; }

QString engineDisplayName(const QString& engine) {
    if (engine.isEmpty()) return QString();
    if (const PseudoEngine* p = pseudoEngine(engine)) return p->label;
    return f::words(engine);
}

DispatchView readDispatch(const Record& record) {
    DispatchView dx;
    const QJsonValue dValue = record.value("dispatch");
    const QJsonObject d = obj(dValue);
    const bool hasDispatch = dValue.isObject();
    const QJsonObject backend = obj(record.value("compute_backend"));
    const QJsonObject stages = obj(record.value("stage_seconds"));
    const QJsonObject termination = obj(record.value("termination"));
    const QString status = str(termination.value("status"));
    const QString message = str(termination.value("message"));

    dx.state = !hasDispatch ? QStringLiteral("not_called")
             : d.value("invoked").toBool(false) ? QStringLiteral("invoked") : QStringLiteral("not_invoked");
    dx.requested = str(obj(record.value("settings")).value("requested_engine"));
    dx.mode = dx.requested.isEmpty() ? QStringLiteral("automatic") : QStringLiteral("forced");
    dx.selected = hasDispatch ? str(d.value("engine")) : QString();
    dx.executed = hasDispatch ? str(d.value("executed_engine")) : QString();
    dx.pseudo = !dx.selected.isEmpty() && isPseudoEngine(dx.selected) ? dx.selected : QString();
    dx.reason = hasDispatch ? str(d.value("reason")) : QString();

    dx.backend.requested = str(backend.value("requested"));
    dx.backend.executed = str(backend.value("executed"));
    dx.backend.requestedDevice = num(backend.value("requested_device"));
    dx.backend.executedDevice = num(backend.value("executed_device"));
    dx.backend.reason = str(backend.value("reason"));
    dx.backend.mismatch = (dx.backend.requested == QLatin1String("cpu") || dx.backend.requested == QLatin1String("cuda")) &&
                          dx.backend.executed != dx.backend.requested;

    dx.selectedNotExecuted = dx.state == QLatin1String("invoked") && dx.pseudo.isEmpty() && !dx.selected.isEmpty() &&
                             dx.executed.isEmpty();
    dx.backendRefusal = dx.selectedNotExecuted && dx.backend.requested == QLatin1String("cuda") && dx.backend.executed.isEmpty();
    dx.engineRejectedModel = dx.selectedNotExecuted && status == QLatin1String("invalid_model") &&
                             record.value("classification").isObject();
    if (dx.selectedNotExecuted) {
        dx.hasRefusal = true;
        dx.refusalMessage = message;
        if (dx.backendRefusal && !dx.backend.reason.isEmpty() && dx.backend.reason != message) {
            dx.refusalBackendReason = dx.backend.reason;
        }
    }

    dx.engineSeconds = num(stages.value("engine"));
    dx.engineTimeKind = !dx.engineSeconds ? QString() : !dx.executed.isEmpty() ? QStringLiteral("execution") : QStringLiteral("path");
    dx.dispatchSeconds = num(stages.value("dispatch"));
    const QJsonValue presolve = record.value("presolve");
    if (dx.state == QLatin1String("invoked") && presolve.isObject()) {
        const QJsonObject p = presolve.toObject();
        dx.hasReducedModel = true;
        dx.reducedVariables = num(p.value("reduced_variables"));
        dx.reducedConstraints = num(p.value("reduced_constraints"));
        dx.reducedNonzeros = num(p.value("reduced_nonzeros"));
    }
    return dx;
}

// ============================================================ result & evidence

ResultFacts resultFacts(const Record& record) {
    ResultFacts r;
    r.dispatch = readDispatch(record);
    const QJsonObject termination = obj(record.value("termination"));
    const QJsonObject self = obj(record.value("self_reported"));
    const QJsonObject validation = obj(record.value("validation"));
    const QJsonValue classification = record.value("classification");
    r.status = str(termination.value("status"));
    r.message = str(termination.value("message"));
    r.sense = str(obj(record.value("instance")).value("objective_sense"));
    r.objective = num(record.value("objective"));
    r.hasPoint = record.value("primal").isArray();
    if (r.hasPoint) r.pointSize = record.value("primal").toArray().size();
    if (classification.isObject()) {
        const QJsonObject c = classification.toObject();
        r.integerModel = c.value("num_binary").toDouble() + c.value("num_integer").toDouble() > 0;
    }
    if (self.value("integrality_respected").isBool()) r.integralityRespected = self.value("integrality_respected").toBool();
    r.integralityViolation = num(self.value("max_integrality_violation"));
    r.hasDuals = record.value("duals").isArray();
    r.dualResidual = num(self.value("max_dual_residual"));
    r.dualsUnavailableReason = str(record.value("duals_unavailable_reason"));
    r.reduced = obj(validation.value("reduced_space"));
    r.original = obj(validation.value("original_space"));
    r.presolveInfeasible = obj(record.value("presolve")).value("infeasible").toBool(false);
    r.executed = r.dispatch.executed;
    r.engine = engineDisplayName(r.executed);
    r.backend = str(obj(record.value("compute_backend")).value("executed"));
    r.totalSeconds = num(obj(record.value("stage_seconds")).value("total"));
    return r;
}

Interpretation interpretResult(const Record& record) {
    const ResultFacts r = resultFacts(record);
    const DispatchView& dx = r.dispatch;
    const QString by = r.executed == QLatin1String("trivial")
                           ? QStringLiteral("the trivial path (solution read from variable bounds)")
                           : (r.engine.isEmpty() ? QStringLiteral("KAIRO") : r.engine);
    const QString kairo = r.message.isEmpty() ? QString() : QStringLiteral(" KAIRO: ") + r.message;
    const QString selected = engineDisplayName(dx.selected);

    if (dx.engineRejectedModel) {
        return {QStringLiteral("engine_rejected"), QStringLiteral("error"),
                dx.mode == QLatin1String("forced") ? QStringLiteral("Requested engine cannot solve this model")
                                                   : QStringLiteral("Selected engine rejected the model"),
                selected + QStringLiteral(" refused the model; the model itself was accepted and classified.") + kairo};
    }
    if (dx.backendRefusal) {
        return {QStringLiteral("backend_refused"), QStringLiteral("error"), QStringLiteral("Requested backend unavailable"),
                QStringLiteral("The dispatcher selected ") + selected +
                    QStringLiteral(", but the requested CUDA backend could not be used, so nothing executed.") + kairo};
    }
    if (dx.selectedNotExecuted) {
        return {QStringLiteral("not_executed"), QStringLiteral("error"), QStringLiteral("Selected engine did not run"),
                QStringLiteral("The dispatcher selected ") + selected + QStringLiteral(", but it did not execute.") + kairo};
    }

    const QString& s = r.status;
    if (s == QLatin1String("optimal")) {
        if (r.integerModel.value_or(false) && r.integralityRespected == std::optional<bool>(false)) {
            return {QStringLiteral("optimal_relaxation"), QStringLiteral("warn"), QStringLiteral("Optimal for the continuous relaxation"),
                    by + QStringLiteral(" solved the continuous relaxation. The integer requirement is not satisfied (max integrality violation ") +
                        f::residual(r.integralityViolation) + QStringLiteral("), so this is not an integer solution.")};
        }
        if (r.integerModel.value_or(false)) {
            return {QStringLiteral("optimal_integer"), QStringLiteral("ok"), QStringLiteral("Optimal — according to the solver"),
                    QStringLiteral("Reported optimal by ") + by +
                        QStringLiteral(". KAIRO validation checked feasibility and integrality of the returned point. "
                                       "Global optimality evidence not independently recorded.")};
        }
        if (r.hasDuals) {
            return {QStringLiteral("optimal_checked"), QStringLiteral("ok"), QStringLiteral("Optimal"),
                    QStringLiteral("Solved by ") + by +
                        QStringLiteral(". KAIRO validation checked the returned point and its optimality (KKT) conditions.")};
        }
        return {QStringLiteral("optimal_reported"), QStringLiteral("ok"), QStringLiteral("Optimal — according to the solver"),
                QStringLiteral("Reported optimal by ") + by +
                    QStringLiteral(". KAIRO validation checked the returned point; optimality conditions were not checked") +
                    (r.dualsUnavailableReason.isEmpty() ? QString() : QStringLiteral(" (") + r.dualsUnavailableReason + QLatin1Char(')')) +
                    QLatin1Char('.')};
    }
    if (s == QLatin1String("limit_reached")) {
        if (r.hasPoint) {
            return {QStringLiteral("limit_point"), QStringLiteral("warn"), QStringLiteral("Limit reached — feasible point returned"),
                    by + QStringLiteral(" stopped at a limit. KAIRO validation checked the returned point; optimality not established.") + kairo};
        }
        return {QStringLiteral("limit_no_point"), QStringLiteral("warn"), QStringLiteral("Limit reached — no solution point available"),
                by + QStringLiteral(" stopped at a limit without a point that passed validation.") + kairo};
    }
    if (s == QLatin1String("infeasible")) {
        if (r.presolveInfeasible && dx.state == QLatin1String("not_invoked")) {
            return {QStringLiteral("infeasible_presolve"), QStringLiteral("warn"), QStringLiteral("Proved infeasible by presolve"),
                    QStringLiteral("Presolve proved the model infeasible before dispatch; no engine ran. No exportable certificate is recorded.")};
        }
        return {QStringLiteral("infeasible_engine"), QStringLiteral("warn"), QStringLiteral("Infeasible — reported by ") + by,
                QStringLiteral("No solution point returned. No infeasibility certificate is recorded, so this is the engine's report.")};
    }
    if (s == QLatin1String("unbounded")) {
        return {QStringLiteral("unbounded_engine"), QStringLiteral("warn"), QStringLiteral("Unbounded — reported by ") + by,
                QStringLiteral("No solution point returned. No unboundedness certificate is recorded, so this is the engine's report.")};
    }
    if (s == QLatin1String("unsupported")) {
        if (dx.pseudo == QLatin1String("unsupported")) {
            return {QStringLiteral("no_engine"), QStringLiteral("error"), QStringLiteral("No suitable engine"),
                    r.message.isEmpty() ? QStringLiteral("No KAIRO engine can solve the model as posed.") : r.message};
        }
        return {QStringLiteral("unsupported"), QStringLiteral("error"), QStringLiteral("Unsupported"), r.message};
    }
    if (s == QLatin1String("invalid_model")) {
        return {QStringLiteral("invalid_model"), QStringLiteral("error"), QStringLiteral("Invalid model"),
                r.message.isEmpty() ? QStringLiteral("KAIRO did not accept the model.") : r.message};
    }
    if (s == QLatin1String("numerical_failure")) {
        const bool rejected = (!r.reduced.isEmpty() && !passed(r.reduced)) || (!r.original.isEmpty() && !passed(r.original));
        return rejected ? Interpretation{QStringLiteral("validation_rejected"), QStringLiteral("error"),
                                         QStringLiteral("Solver failure — KAIRO validation rejected the engine's point"), r.message}
                        : Interpretation{QStringLiteral("solver_failure"), QStringLiteral("error"), QStringLiteral("Solver failure"), r.message};
    }
    return {QStringLiteral("other"), QStringLiteral("error"), f::words(s), r.message};
}

QString evidenceStateLabel(const QString& state) {
    static const QHash<QString, QString> labels{
        {"checked", "Checked"}, {"failed", "Failed"}, {"not_checked", "Not checked"}, {"not_available", "Not available"},
        {"not_applicable", "Not applicable"}, {"nothing", "Nothing to validate"}, {"not_established", "Not established"},
        {"reported", "Reported by engine"}, {"proved_presolve", "Proved by presolve"}, {"returned", "Returned"}, {"none", "None"}};
    return labels.value(state);
}

QString evidenceStateTone(const QString& state) {
    static const QHash<QString, QString> tones{
        {"checked", "ok"}, {"failed", "error"}, {"not_checked", "muted"}, {"not_available", "muted"}, {"not_applicable", "muted"},
        {"nothing", "muted"}, {"not_established", "warn"}, {"reported", "neutral"}, {"proved_presolve", "neutral"},
        {"returned", "ok"}, {"none", "muted"}};
    return tones.value(state);
}

namespace {

EvidenceItem item(const QString& key, const QString& label, const QString& state, const QString& text,
                  const QString& detail = QString()) {
    return {key, label, state, evidenceStateLabel(state), evidenceStateTone(state), text, detail};
}

QString residualText(const QJsonObject& check) {
    return QStringLiteral("max bound violation %1, max constraint violation %2 (scaled %3 · %4)")
        .arg(f::residual(num(check.value("max_bound_residual"))), f::residual(num(check.value("max_constraint_residual"))),
             f::residual(num(check.value("max_bound_residual_scaled"))), f::residual(num(check.value("max_constraint_residual_scaled"))));
}

EvidenceItem checkItem(const QString& key, const QString& label, const QJsonObject& check, bool hasPoint, const QString& passedText) {
    if (!check.isEmpty()) {
        if (passed(check)) return item(key, label, "checked", passedText, residualText(check));
        const QString failure = str(check.value("failure"));
        return item(key, label, "failed", f::words(str(check.value("status"))) + (failure.isEmpty() ? QString() : QStringLiteral(": ") + failure),
                    QStringLiteral("Residuals of a failed check are not recorded."));
    }
    return hasPoint ? item(key, label, "not_checked", QStringLiteral("This check did not run."))
                    : item(key, label, "nothing", QStringLiteral("No solution point was returned."));
}

}  // namespace

QList<EvidenceItem> buildEvidence(const Record& record) {
    const ResultFacts r = resultFacts(record);
    QList<EvidenceItem> items;
    const QString engine = r.engine.isEmpty() ? QStringLiteral("the engine") : r.engine;

    items.append(r.hasPoint ? item("solution", "Solution point", "returned", f::count(r.pointSize) + QStringLiteral(" values, in original-model coordinates."))
                            : item("solution", "Solution point", "none", QStringLiteral("No solution point returned.")));
    items.append(checkItem("reduced", "Engine point (reduced model)", r.reduced, r.hasPoint,
                           QStringLiteral("Verified by KAIRO validation against the presolved model.")));
    EvidenceItem original = checkItem("feasibility", "Primal feasibility (original model)", r.original, r.hasPoint,
                                      QStringLiteral("Verified by KAIRO validation after postsolve: bounds and constraints of the original model."));
    if (!r.original.isEmpty() && passed(r.original) && r.integralityRespected == std::optional<bool>(false)) {
        original.text += QStringLiteral(" Integrality is not satisfied — see Integrality.");
    }
    items.append(original);

    if (!r.integerModel) items.append(item("integrality", "Integrality", "not_available", QStringLiteral("The model was not classified.")));
    else if (!*r.integerModel) items.append(item("integrality", "Integrality", "not_applicable", QStringLiteral("Continuous model.")));
    else if (!r.hasPoint) items.append(item("integrality", "Integrality", "nothing", QStringLiteral("No solution point was returned.")));
    else if (r.integralityRespected == std::optional<bool>(true)) {
        items.append(item("integrality", "Integrality", "checked", QStringLiteral("Integer requirement satisfied."),
                          QStringLiteral("max integrality violation ") + f::residual(r.integralityViolation)));
    } else {
        items.append(item("integrality", "Integrality", "failed", QStringLiteral("Integer requirement not satisfied."),
                          QStringLiteral("max integrality violation ") + f::residual(r.integralityViolation)));
    }

    const bool originalPassed = !r.original.isEmpty() && passed(r.original);
    const bool reducedPassed = !r.reduced.isEmpty() && passed(r.reduced);
    if (!r.hasPoint) items.append(item("objective", "Objective", "nothing", QStringLiteral("No solution point was returned.")));
    else if (originalPassed || reducedPassed) {
        const Number engineValue = num(r.reduced.value("engine_reported_objective"));
        const Number recomputed = originalPassed ? num(r.original.value("objective")) : num(r.reduced.value("objective"));
        const bool differs = engineValue && engineValue != recomputed;
        QString text = QStringLiteral("Recomputed by KAIRO from the point: ") + f::real(recomputed) + QLatin1Char('.');
        if (engineValue) {
            text += QStringLiteral(" The engine reported ") + f::real(engineValue) +
                    (differs ? QStringLiteral(", which differs from the recomputed value") : QString()) + QLatin1Char('.');
        }
        QStringList detail;
        if (reducedPassed) detail << QStringLiteral("reduced model ") + f::real(num(r.reduced.value("objective")));
        if (originalPassed) detail << QStringLiteral("original model ") + f::real(num(r.original.value("objective")));
        items.append(item("objective", "Objective", "checked", text, detail.join(QStringLiteral(" · "))));
    } else {
        items.append(item("objective", "Objective", "not_checked", QStringLiteral("No validation recomputed the objective.")));
    }

    if (r.integerModel.value_or(false)) {
        if (r.status == QLatin1String("optimal") && r.integralityRespected == std::optional<bool>(true)) {
            items.append(item("optimality", "Global optimality", "reported",
                              QStringLiteral("Reported optimal by ") + engine + QStringLiteral(". Global optimality evidence not independently recorded.")));
        } else if (r.status == QLatin1String("optimal")) {
            items.append(item("optimality", "Integer optimality", "not_established",
                              QStringLiteral("Optimal for the continuous relaxation only; integer optimality not established.")));
        } else if (r.status == QLatin1String("limit_reached") && r.hasPoint) {
            items.append(item("optimality", "Global optimality", "not_established", QStringLiteral("Optimality not established: the run stopped at a limit.")));
        }
        if (r.hasPoint) {
            items.append(item("bound", "Dual bound / optimality gap", "not_available", QStringLiteral("Not recorded by KAIRO for integer models.")));
        }
    } else if (r.integerModel == std::optional<bool>(false) && r.hasPoint) {
        if (r.hasDuals) {
            items.append(item("optimality", "Optimality conditions (KKT)", "checked",
                              QStringLiteral("Verified by KAIRO validation: stationarity, sign conditions and complementary slackness of the reconstructed duals."),
                              QStringLiteral("max dual residual ") + f::residual(r.dualResidual)));
        } else if (r.status == QLatin1String("optimal")) {
            items.append(item("optimality", "Optimality conditions (KKT)", "not_checked",
                              r.dualsUnavailableReason.isEmpty() ? QStringLiteral("Optimality conditions were not checked.") : r.dualsUnavailableReason));
        } else if (r.status == QLatin1String("limit_reached")) {
            items.append(item("optimality", "Optimality", "not_established", QStringLiteral("Optimality not established: the run stopped at a limit.")));
        }
    }

    if (r.status == QLatin1String("infeasible")) {
        items.append(r.presolveInfeasible && r.dispatch.state == QLatin1String("not_invoked")
                         ? item("infeasibility", "Infeasibility", "proved_presolve",
                                QStringLiteral("Proved by presolve before dispatch. No exportable certificate is recorded."))
                         : item("infeasibility", "Infeasibility", "reported",
                                QStringLiteral("Reported by ") + engine + QStringLiteral(". No certificate is recorded.")));
    }
    if (r.status == QLatin1String("unbounded")) {
        items.append(item("unboundedness", "Unboundedness", "reported", QStringLiteral("Reported by ") + engine + QStringLiteral(". No certificate is recorded.")));
    }
    return items;
}

Headline summarize(const Record& record) {
    const Interpretation i = interpretResult(record);
    return {i.tone, i.title, i.detail, str(obj(record.value("termination")).value("status"))};
}

// ============================================================ pipeline

namespace {

QString stageTone(const QString& state) {
    static const QHash<QString, QString> tones{{"completed", "ok"},   {"not_run", "muted"},      {"failed", "error"},
                                               {"unsupported", "error"}, {"refused", "error"},  {"infeasible", "warn"},
                                               {"unbounded", "warn"},    {"limit", "warn"}};
    return tones.value(state);
}

PipelineStage stage(const QString& id, const QString& label, const QString& target, const QString& state, const QString& stateLabel,
                    const QStringList& lines = {}, Number seconds = std::nullopt, const QString& timeLabel = QString()) {
    PipelineStage s;
    s.id = id;
    s.label = label;
    s.target = target;
    s.state = state;
    s.tone = stageTone(state);
    s.stateLabel = stateLabel;
    for (const QString& line : lines) if (!line.isEmpty()) s.lines << line;
    s.seconds = seconds;
    s.timeLabel = timeLabel;
    return s;
}

PipelineStage notRunStage(const QString& id, const QString& label, const QString& target,
                          const QString& stateLabel = QStringLiteral("not run"), const QStringList& lines = {}) {
    return stage(id, label, target, QStringLiteral("not_run"), stateLabel, lines);
}

}  // namespace

Pipeline buildPipeline(const Record& record) {
    const QJsonObject term = obj(record.value("termination"));
    const QString status = str(term.value("status"));
    const QString message = str(term.value("message"));
    const QJsonObject st = obj(record.value("stage_seconds"));
    const QJsonObject instance = obj(record.value("instance"));
    const QJsonValue cValue = record.value("classification");
    const QJsonValue pValue = record.value("presolve");
    const QJsonValue dValue = record.value("dispatch");
    const QJsonObject v = obj(record.value("validation"));
    Pipeline pipeline;
    QList<PipelineStage>& stages = pipeline.stages;

    const bool refusedModel = status == QLatin1String("invalid_model") && !cValue.isObject();
    if (refusedModel) {
        stages << stage("model", "Model", "model-analysis", "failed",
                        absent(instance.value("variables")) ? QStringLiteral("could not be read") : QStringLiteral("rejected by KAIRO"),
                        {message}, num(st.value("parse")), QStringLiteral("read"));
    } else {
        const QString sense = str(instance.value("objective_sense"));
        stages << stage("model", "Model", "model-analysis", "completed", QStringLiteral("accepted"),
                        {plural(num(instance.value("variables")), "variable", "variables") + QStringLiteral(" · ") +
                             plural(num(instance.value("constraints")), "constraint", "constraints"),
                         sense == QLatin1String("max") ? QStringLiteral("maximize") : sense == QLatin1String("min") ? QStringLiteral("minimize") : QString()},
                        num(st.value("parse")), QStringLiteral("read"));
    }

    if (cValue.isObject()) {
        const QJsonObject c = cValue.toObject();
        QStringList types;
        for (const auto& [key, word] : {std::pair{"num_continuous", "continuous"}, {"num_integer", "integer"}, {"num_binary", "binary"}}) {
            const Number n = num(c.value(key));
            if (n && *n != 0) types << f::count(n) + QLatin1Char(' ') + QLatin1String(word);
        }
        stages << stage("classification", "Classification", "model-analysis", "completed", str(c.value("problem_class")),
                        {types.join(QStringLiteral(" · ")), f::count(num(c.value("nonzeros"))) + QStringLiteral(" nonzeros")},
                        num(st.value("classification")));
    } else {
        stages << notRunStage("classification", "Classification", "model-analysis");
    }

    if (pValue.isObject()) {
        const QJsonObject p = pValue.toObject();
        const bool infeasible = p.value("infeasible").toBool(false);
        stages << stage("presolve", "Presolve", "presolve-impact", infeasible ? "infeasible" : "completed",
                        infeasible ? QStringLiteral("proved infeasible")
                                   : p.value("converged").toBool(false) ? QStringLiteral("completed") : QStringLiteral("stopped at pass limit"),
                        {f::count(num(p.value("original_variables"))) + QStringLiteral(" → ") + f::count(num(p.value("reduced_variables"))) + QStringLiteral(" variables"),
                         f::count(num(p.value("original_constraints"))) + QStringLiteral(" → ") + f::count(num(p.value("reduced_constraints"))) + QStringLiteral(" constraints")},
                        num(st.value("presolve")));
    } else {
        stages << notRunStage("presolve", "Presolve", "presolve-impact");
    }

    const QJsonObject d = obj(dValue);
    const bool invoked = d.value("invoked").toBool(false);
    const QString dEngine = str(d.value("engine"));
    const bool noEngine = invoked && dEngine == QLatin1String("unsupported");
    if (!dValue.isObject()) stages << notRunStage("dispatch", "Dispatch", "dispatch");
    else if (!invoked) stages << notRunStage("dispatch", "Dispatch", "dispatch", QStringLiteral("not invoked"));
    else if (noEngine) stages << stage("dispatch", "Dispatch", "dispatch", "unsupported", QStringLiteral("no suitable engine"), {str(d.value("reason"))}, num(st.value("dispatch")));
    else stages << stage("dispatch", "Dispatch", "dispatch", "completed", QStringLiteral("invoked"),
                         {QStringLiteral("selected: ") + engineDisplayName(dEngine)}, num(st.value("dispatch")));

    const QString executed = str(d.value("executed_engine"));
    const QString backend = str(obj(record.value("compute_backend")).value("executed"));
    if (!executed.isEmpty()) {
        static const QHash<QString, QPair<QString, QString>> engineStates{
            {"optimal", {"completed", "executed"}},          {"infeasible", {"infeasible", "proved infeasible"}},
            {"unbounded", {"unbounded", "proved unbounded"}}, {"limit_reached", {"limit", "stopped at a limit"}},
            {"numerical_failure", {"failed", "numerical failure"}}, {"invalid_model", {"failed", "refused the model"}}};
        const auto state = engineStates.value(status, {QStringLiteral("failed"), f::words(status)});
        stages << stage("engine", "Engine", "execution", state.first, state.second,
                        {QStringLiteral("executed: ") + engineDisplayName(executed),
                         backend.isEmpty() ? QString() : QStringLiteral("backend: ") + backend.toUpper()},
                        num(st.value("engine")));
    } else if (invoked && !noEngine) {
        stages << stage("engine", "Engine", "execution", "refused", QStringLiteral("not executed"),
                        {QStringLiteral("selected: ") + engineDisplayName(dEngine), QStringLiteral("executed: none"), message},
                        num(st.value("engine")), QStringLiteral("engine path"));
    } else {
        stages << notRunStage("engine", "Engine", "execution", QStringLiteral("not run"), {QStringLiteral("nothing executed")});
    }

    const QJsonObject original = obj(v.value("original_space"));
    const QJsonObject reduced = obj(v.value("reduced_space"));
    if (original.isEmpty()) stages << notRunStage("postsolve", "Postsolve", "validation");
    else if (str(original.value("status")) == QLatin1String("invalid_mapping") || str(original.value("status")) == QLatin1String("internal_error")) {
        stages << stage("postsolve", "Postsolve", "validation", "failed", QStringLiteral("reconstruction failed"), {str(original.value("failure"))}, num(st.value("postsolve")));
    } else {
        stages << stage("postsolve", "Postsolve", "validation", "completed", QStringLiteral("completed"), {QStringLiteral("original coordinates")}, num(st.value("postsolve")));
    }

    QList<QPair<QString, QJsonObject>> checks;
    if (!reduced.isEmpty()) checks << qMakePair(QStringLiteral("reduced space"), reduced);
    if (!original.isEmpty()) checks << qMakePair(QStringLiteral("original space"), original);
    if (checks.isEmpty()) stages << notRunStage("validation", "Validation", "validation");
    else {
        bool failed = false;
        QStringList lines;
        for (const auto& [space, check] : checks) {
            failed = failed || !passed(check);
            lines << space + QStringLiteral(": ") + (passed(check) ? QStringLiteral("passed") : f::words(str(check.value("status"))));
        }
        stages << stage("validation", "Validation", "validation", failed ? "failed" : "completed",
                        failed ? QStringLiteral("failed") : QStringLiteral("passed"), lines, num(st.value("reduced_validation")),
                        QStringLiteral("reduced check"));
    }

    static const QHash<QString, QPair<QString, QString>> resultStates{
        {"optimal", {"completed", "optimal"}},    {"infeasible", {"infeasible", "infeasible"}},
        {"unbounded", {"unbounded", "unbounded"}}, {"limit_reached", {"limit", "limit reached"}},
        {"unsupported", {"unsupported", "unsupported"}}, {"invalid_model", {"failed", "invalid model"}},
        {"numerical_failure", {"failed", "solver failure"}}};
    const auto result = readDispatch(record).engineRejectedModel
                            ? QPair<QString, QString>(QStringLiteral("failed"), QStringLiteral("engine rejected model"))
                            : resultStates.value(status, {QStringLiteral("failed"), f::words(status)});
    const Number objective = num(record.value("objective"));
    stages << stage("result", "Result", "result", result.first, result.second,
                    {objective ? QStringLiteral("objective ") + f::real(objective) : QStringLiteral("no point returned")},
                    num(st.value("total")), QStringLiteral("total"));

    int end = -1;
    for (int i = 0; i < stages.size() - 1; ++i) if (stages[i].state != QLatin1String("not_run")) end = i;
    if (end >= 0) {
        stages[end].terminal = true;
        pipeline.terminatedAt = stages[end].id;
        pipeline.summary = result.first == QLatin1String("completed")
                               ? QStringLiteral("Ran end to end: %1 %2.").arg(stages[end].label.toLower(), stages[end].stateLabel)
                               : QStringLiteral("Ended at %1: %2.").arg(stages[end].label, stages[end].stateLabel);
    }
    return pipeline;
}

// ============================================================ model & presolve

ModelAnalysis buildModelAnalysis(const Record& record) {
    ModelAnalysis a;
    const QJsonValue cValue = record.value("classification");
    const QJsonObject instance = obj(record.value("instance"));
    const QString senseValue = str(instance.value("objective_sense"));
    a.sense = senseValue == QLatin1String("max") ? QStringLiteral("Maximize") : senseValue == QLatin1String("min") ? QStringLiteral("Minimize") : QString();
    if (cValue.isObject()) {
        const QJsonObject c = cValue.toObject();
        a.state = QStringLiteral("classified");
        a.problemClass = str(c.value("problem_class"));
        a.variables = num(c.value("num_columns"));
        a.constraints = num(c.value("num_rows"));
        a.nonzeros = num(c.value("nonzeros"));
        a.continuous = num(c.value("num_continuous"));
        a.integer = num(c.value("num_integer"));
        a.binary = num(c.value("num_binary"));
        // Only flags the record carries are shown.
        if (c.value("has_network_structure").isBool()) {
            a.structure << StructureFlag{"network", "Network structure", c.value("has_network_structure").toBool(), QString()};
        }
        if (c.value("has_big_m").isBool()) {
            const bool bigM = c.value("has_big_m").toBool();
            a.structure << StructureFlag{"big_m", "Big-M rows", bigM,
                                         bigM && !absent(c.value("max_big_m")) ? QStringLiteral("max ") + f::real(num(c.value("max_big_m"))) : QString()};
        }
        if (c.value("has_set_partitioning").isBool()) {
            a.structure << StructureFlag{"set_partitioning", "Set partitioning", c.value("has_set_partitioning").toBool(), QString()};
        }
        if (c.value("symmetric_groups").isDouble()) {
            const double groups = c.value("symmetric_groups").toDouble();
            a.structure << StructureFlag{"symmetry", "Symmetric column groups", groups > 0, groups > 0 ? f::count(groups) : QString()};
        }
        if (a.nonzeros.value_or(0) > 0) a.coefRangeRatio = num(c.value("coef_range_ratio"));
        return a;
    }
    if (absent(instance.value("variables"))) {
        a.state = QStringLiteral("unreadable");
        a.sense.clear();
        a.message = str(obj(record.value("termination")).value("message"));
        return a;
    }
    a.state = QStringLiteral("unclassified");
    a.reason = str(obj(record.value("termination")).value("status")) == QLatin1String("invalid_model")
                   ? QStringLiteral("KAIRO did not accept the model") : QStringLiteral("classification did not run");
    a.variables = num(instance.value("variables"));
    a.constraints = num(instance.value("constraints"));
    return a;
}

Number reductionPercent(Number original, Number reduced) {
    if (!original || !(*original > 0) || !reduced) return std::nullopt;
    return 100.0 * (*original - *reduced) / *original;
}

PresolveImpact buildPresolveImpact(const Record& record) {
    PresolveImpact impact;
    impact.seconds = num(obj(record.value("stage_seconds")).value("presolve"));
    const QJsonValue pValue = record.value("presolve");
    if (!pValue.isObject()) {
        impact.state = QStringLiteral("not_run");
        impact.stateLabel = f::kNotRun;
        if (!record.value("classification").isObject()) impact.reason = QStringLiteral("the model never reached presolve");
        return impact;
    }
    const QJsonObject p = pValue.toObject();
    // An infeasible run stops presolve mid-way: its "reduced" numbers are the
    // dimensions at that moment, and its converged flag says nothing.
    impact.state = p.value("infeasible").toBool(false) ? QStringLiteral("infeasible")
                 : p.value("converged").toBool(false) ? QStringLiteral("converged") : QStringLiteral("not_converged");
    impact.stopped = impact.state == QLatin1String("infeasible");
    impact.stateLabel = impact.stopped ? QStringLiteral("Proved infeasible")
                      : impact.state == QLatin1String("converged") ? QStringLiteral("Converged")
                                                                   : QStringLiteral("Stopped before convergence (pass limit)");
    impact.heading = impact.stopped ? QStringLiteral("Dimensions when presolve stopped") : QStringLiteral("Original → reduced");
    for (const auto& [key, label, o, r] : {std::tuple{"variables", "Variables", "original_variables", "reduced_variables"},
                                           std::tuple{"constraints", "Constraints", "original_constraints", "reduced_constraints"},
                                           std::tuple{"nonzeros", "Nonzeros", "original_nonzeros", "reduced_nonzeros"}}) {
        ImpactRow row;
        row.key = QLatin1String(key);
        row.label = QLatin1String(label);
        row.original = num(p.value(o));
        row.reduced = num(p.value(r));
        if (row.original && row.reduced) row.change = *row.reduced - *row.original;
        if (!impact.stopped) row.reduction = reductionPercent(row.original, row.reduced);
        impact.rows << row;
    }
    impact.transformations = num(p.value("transformations"));
    const QJsonObject byType = obj(p.value("transformations_by_type"));
    // Record order, as the JSON writer emits it.
    for (const char* type : {"remove_variable", "remove_constraint", "fix_variable", "substitute_variable",
                             "tighten_lower_bound", "tighten_upper_bound"}) {
        const double count = byType.value(QLatin1String(type)).toDouble();
        if (count > 0) impact.byType << qMakePair(QString::fromLatin1(type), count);
    }
    return impact;
}

}  // namespace kairo::record
