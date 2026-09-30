#include "barrier/barrier_adapter.h"

#include <cmath>

namespace barrier {

Translation toBarrierProblem(const model::Model& model, BarrierProblem& problem) {
    Translation t;
    problem = BarrierProblem{};
    const auto n = static_cast<Index>(model.variables.size());
    const auto m = static_cast<Index>(model.constraints.size());
    t.negated = model.objective.sense == model::ObjectiveSense::Maximize;
    const double sign = t.negated ? -1.0 : 1.0;

    // ---- variables: keep or substitute out --------------------------------
    t.columnOf.assign(static_cast<std::size_t>(n), -1);
    t.fixedValue.assign(static_cast<std::size_t>(n), 0.0);
    Index columns = 0;
    for (Index j = 0; j < n; ++j) {
        const auto& v = model.variables[static_cast<std::size_t>(j)];
        const double l = v.lowerBound, u = v.upperBound;
        if (std::isnan(l) || std::isnan(u)) { t.error = "variable bound is NaN"; return t; }
        if (l > u) { t.error = "variable '" + v.name + "' has crossed bounds"; return t; }
        if (l == u) {
            if (!std::isfinite(l)) { t.error = "variable '" + v.name + "' is fixed at infinity"; return t; }
            t.fixedValue[static_cast<std::size_t>(j)] = l;
            ++t.fixedVariables;
        } else {
            t.columnOf[static_cast<std::size_t>(j)] = columns++;
        }
    }
    t.structuralColumns = columns;
    const auto isFixed = [&](Index j) { return t.columnOf[static_cast<std::size_t>(j)] < 0; };
    const auto fixedAt = [&](Index j) { return t.fixedValue[static_cast<std::size_t>(j)]; };

    // ---- objective --------------------------------------------------------
    std::vector<double> objective(static_cast<std::size_t>(columns), 0.0);
    double offset = sign * model.objective.offset;
    for (const auto& term : model.objective.linearTerms) {
        if (term.variableIndex < 0 || term.variableIndex >= n) {
            t.error = "linear objective index out of range"; return t;
        }
        const double c = sign * term.value;
        if (isFixed(term.variableIndex)) offset += c * fixedAt(term.variableIndex);
        else objective[static_cast<std::size_t>(t.columnOf[static_cast<std::size_t>(term.variableIndex)])] += c;
    }

    TripletBuilder hessian(columns, columns);
    bool anyCurvature = false;
    for (const auto& term : model.objective.quadraticTerms) {
        const Index a = term.variableIndex1, b = term.variableIndex2;
        if (a < 0 || a >= n || b < 0 || b >= n) {
            t.error = "quadratic objective index out of range"; return t;
        }
        const double q = sign * term.value;  // coefficient of x_a * x_b, no implicit 1/2
        if (q == 0.0) continue;
        if (a == b) {
            if (isFixed(a)) { offset += q * fixedAt(a) * fixedAt(a); continue; }
            const Index ca = t.columnOf[static_cast<std::size_t>(a)];
            hessian.add(ca, ca, 2.0 * q);  // 0.5 * (2q) x^2 = q x^2
            anyCurvature = true;
            continue;
        }
        const bool fa = isFixed(a), fb = isFixed(b);
        if (fa && fb) { offset += q * fixedAt(a) * fixedAt(b); continue; }
        if (fa) { objective[static_cast<std::size_t>(t.columnOf[static_cast<std::size_t>(b)])] += q * fixedAt(a); continue; }
        if (fb) { objective[static_cast<std::size_t>(t.columnOf[static_cast<std::size_t>(a)])] += q * fixedAt(b); continue; }
        Index ca = t.columnOf[static_cast<std::size_t>(a)], cb = t.columnOf[static_cast<std::size_t>(b)];
        if (ca > cb) std::swap(ca, cb);
        // Upper triangle only: 0.5 * (Q_ab + Q_ba) x_a x_b = Q_ab x_a x_b when
        // the solver mirrors it, so Q_ab = q exactly.
        hessian.add(ca, cb, q);
        anyCurvature = true;
    }

    // ---- rows -------------------------------------------------------------
    t.rowOf.assign(static_cast<std::size_t>(m), -1);
    struct Row { std::vector<std::pair<Index, double>> entries; double rhs; bool slack; double lo, hi; };
    std::vector<Row> rows;
    rows.reserve(static_cast<std::size_t>(m));
    for (Index i = 0; i < m; ++i) {
        const auto& c = model.constraints[static_cast<std::size_t>(i)];
        const double rl = c.lowerBound, ru = c.upperBound;
        if (std::isnan(rl) || std::isnan(ru)) { t.error = "constraint bound is NaN"; return t; }
        if (rl > ru) { t.error = "constraint '" + c.name + "' has crossed bounds"; return t; }
        if (!std::isfinite(rl) && !std::isfinite(ru)) { ++t.droppedRows; continue; }  // constrains nothing

        Row row{};
        double fixedActivity = 0.0;
        for (const auto& term : c.linearTerms) {
            if (term.variableIndex < 0 || term.variableIndex >= n) {
                t.error = "constraint index out of range"; return t;
            }
            if (isFixed(term.variableIndex)) fixedActivity += term.value * fixedAt(term.variableIndex);
            else row.entries.emplace_back(t.columnOf[static_cast<std::size_t>(term.variableIndex)], term.value);
        }
        if (rl == ru) {
            row.slack = false;
            row.rhs = rl - fixedActivity;
        } else {
            // a'x - s = 0 with rl - fixed <= s <= ru - fixed. The shift is a
            // constant, so it moves the slack's box but not the multiplier.
            row.slack = true;
            row.rhs = 0.0;
            row.lo = std::isfinite(rl) ? rl - fixedActivity : -infinity;
            row.hi = std::isfinite(ru) ? ru - fixedActivity : infinity;
        }
        t.rowOf[static_cast<std::size_t>(i)] = static_cast<Index>(rows.size());
        rows.push_back(std::move(row));
    }

    Index slacks = 0;
    for (const auto& row : rows) slacks += row.slack ? 1 : 0;
    t.slackColumns = static_cast<std::size_t>(slacks);
    const Index total = columns + slacks;
    const auto barrierRows = static_cast<Index>(rows.size());

    TripletBuilder a(barrierRows, total);
    problem.rightHandSide.assign(static_cast<std::size_t>(barrierRows), 0.0);
    problem.lower.assign(static_cast<std::size_t>(total), 0.0);
    problem.upper.assign(static_cast<std::size_t>(total), 0.0);
    problem.objective.assign(static_cast<std::size_t>(total), 0.0);
    for (Index j = 0; j < n; ++j) {
        const Index col = t.columnOf[static_cast<std::size_t>(j)];
        if (col < 0) continue;
        problem.lower[static_cast<std::size_t>(col)] = model.variables[static_cast<std::size_t>(j)].lowerBound;
        problem.upper[static_cast<std::size_t>(col)] = model.variables[static_cast<std::size_t>(j)].upperBound;
        problem.objective[static_cast<std::size_t>(col)] = objective[static_cast<std::size_t>(col)];
    }
    Index nextSlack = columns;
    for (Index r = 0; r < barrierRows; ++r) {
        const Row& row = rows[static_cast<std::size_t>(r)];
        for (const auto& [col, value] : row.entries) a.add(r, col, value);
        problem.rightHandSide[static_cast<std::size_t>(r)] = row.rhs;
        if (row.slack) {
            a.add(r, nextSlack, -1.0);
            problem.lower[static_cast<std::size_t>(nextSlack)] = row.lo;
            problem.upper[static_cast<std::size_t>(nextSlack)] = row.hi;
            ++nextSlack;
        }
    }
    problem.constraints = a.build();
    if (anyCurvature) {
        // The Hessian is sized to the structural columns; widen it to include
        // the slacks, which carry no curvature.
        TripletBuilder widened(total, total);
        const SparseCsc q = hessian.build();
        for (Index j = 0; j < q.cols; ++j)
            for (Index p = q.columnStart[j]; p < q.columnStart[j + 1]; ++p)
                widened.add(q.rowIndex[p], j, q.value[p]);
        problem.hessian = widened.build();
    }
    problem.objectiveOffset = offset;
    t.ok = true;
    return t;
}

ModelSolution toModelSolution(const model::Model& model, const Translation& translation,
                              const Result& result) {
    ModelSolution out;
    const auto n = model.variables.size();
    const auto m = model.constraints.size();
    if (!translation.ok || !result.hasSolution) return out;

    out.variableValues.assign(n, 0.0);
    for (std::size_t j = 0; j < n; ++j) {
        const Index col = translation.columnOf[j];
        out.variableValues[j] = col < 0 ? translation.fixedValue[j]
                                        : result.primal[static_cast<std::size_t>(col)];
    }

    // y is the minimisation's shadow price for its row (see the header);
    // undo the maximisation negation once.
    const double back = translation.negated ? -1.0 : 1.0;
    out.constraintDuals.assign(m, 0.0);
    for (std::size_t i = 0; i < m; ++i) {
        const Index row = translation.rowOf[i];
        if (row >= 0) out.constraintDuals[i] = back * result.equalityDual[static_cast<std::size_t>(row)];
    }

    // Recompute the objective from the model itself rather than transforming
    // the solver's: that way an error in the offset or sign bookkeeping above
    // cannot reach the reported value.
    double objective = model.objective.offset;
    for (const auto& term : model.objective.linearTerms)
        objective += term.value * out.variableValues[static_cast<std::size_t>(term.variableIndex)];
    for (const auto& term : model.objective.quadraticTerms)
        objective += term.value * out.variableValues[static_cast<std::size_t>(term.variableIndex1)] *
                     out.variableValues[static_cast<std::size_t>(term.variableIndex2)];
    out.objectiveValue = objective;
    return out;
}

}  // namespace barrier
