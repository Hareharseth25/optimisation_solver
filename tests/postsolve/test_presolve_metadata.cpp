#include "model/model.h"
#include "presolve/presolver.h"
#include "test_helpers.h"

#include <iostream>

using namespace test_helpers;

// ============================================================
// Fixed variable recorded in PostsolveMetadata
// ============================================================

void test_fixed_variable_recorded() {
    const std::string testName = "test_fixed_variable_recorded";

    model::Model original;
    original.name = "FixedVariableModel";

    original.variables.push_back(makeVariable("x", 5.0, 5.0));
    original.variables.push_back(makeVariable("y", 0.0, 10.0));

    original.constraints.push_back(
        makeConstraint("C1", -100.0, 100.0, { term(0, 1.0), term(1, 1.0) })
    );

    presolve::Presolver presolver;
    presolve::PresolveResult result = presolver.run(original);

    checkTrue(testName, "model is not infeasible", !result.infeasible);

    checkTrue(
        testName,
        "at least one fixed variable was recorded",
        !result.postsolve.fixedVariables.empty()
    );

    bool foundX = false;
    for (const auto& fv : result.postsolve.fixedVariables) {
        if (fv.originalIndex == 0) {
            foundX = true;
            checkNearlyEqual(testName, "fixedValue for x", 5.0, fv.fixedValue);
            checkTrue(testName, "recorded name is 'x'", fv.name == "x");
        }
    }

    checkTrue(testName, "fixed variable record for original index 0 (x) found",
              foundX);

    checkTrue(
        testName,
        "originalToPresolvedVar has an entry for x",
        result.postsolve.originalToPresolvedVar.size() > 0
    );

    if (!result.postsolve.originalToPresolvedVar.empty()) {
        checkTrue(
            testName,
            "x (original index 0) maps to -1 (eliminated) in originalToPresolvedVar",
            result.postsolve.originalToPresolvedVar[0] == -1
        );
    }

    std::cout << "[PASS] " << testName << "\n";
}

// ============================================================
// Singleton equality — positive and negative coefficient
// ============================================================

void test_singleton_equality_positive_coefficient() {
    const std::string testName = "test_singleton_equality_positive_coefficient";

    model::Model original;
    original.name = "SingletonPositive";
    original.variables.push_back(makeVariable("x", -1000.0, 1000.0));
    original.constraints.push_back(
        makeConstraint("C1", 10.0, 10.0, { term(0, 2.0) })  // 2x = 10
    );

    presolve::Presolver presolver;
    presolve::PresolveResult result = presolver.run(original);

    checkTrue(testName, "model is not infeasible", !result.infeasible);

    bool found = false;
    for (const auto& fv : result.postsolve.fixedVariables) {
        if (fv.originalIndex == 0) {
            found = true;
            checkNearlyEqual(testName, "x from 2x=10", 5.0, fv.fixedValue);
        }
    }
    checkTrue(testName, "x was fixed via singleton row", found);

    std::cout << "[PASS] " << testName << "\n";
}

void test_singleton_equality_negative_coefficient() {
    const std::string testName = "test_singleton_equality_negative_coefficient";

    model::Model original;
    original.name = "SingletonNegative";
    original.variables.push_back(makeVariable("x", -1000.0, 1000.0));
    original.constraints.push_back(
        makeConstraint("C1", -10.0, -10.0, { term(0, -2.0) })  // -2x = -10
    );

    presolve::Presolver presolver;
    presolve::PresolveResult result = presolver.run(original);

    checkTrue(testName, "model is not infeasible", !result.infeasible);

    bool found = false;
    for (const auto& fv : result.postsolve.fixedVariables) {
        if (fv.originalIndex == 0) {
            found = true;
            checkNearlyEqual(
                testName,
                "x from -2x=-10 (sign check)",
                5.0,
                fv.fixedValue
            );
        }
    }
    checkTrue(testName, "x was fixed via singleton row with negative coefficient",
              found);

    std::cout << "[PASS] " << testName << "\n";
}

// ============================================================
// Bound tightening recorded
// ============================================================

void test_bound_tightening_recorded() {
    const std::string testName = "test_bound_tightening_recorded";

    model::Model original;
    original.name = "BoundTighteningModel";
    original.variables.push_back(makeVariable("x", 0.0, 20.0));

    original.constraints.push_back(
        makeConstraint("C1", 5.0, 20.0, { term(0, 1.0) })
    );

    presolve::Presolver presolver;
    presolve::PresolveResult result = presolver.run(original);

    checkTrue(testName, "model is not infeasible", !result.infeasible);

    checkNearlyEqual(
        testName,
        "original.variables[0].lowerBound unchanged",
        0.0,
        original.variables[0].lowerBound
    );
    checkNearlyEqual(
        testName,
        "original.variables[0].upperBound unchanged",
        20.0,
        original.variables[0].upperBound
    );

    bool foundTightening = false;
    for (const auto& t : result.transformations) {
        if (
            t.type == presolve::TransformationType::TightenLowerBound &&
            t.originalVariableIndex == 0
        ) {
            foundTightening = true;
            checkNearlyEqual(
                testName,
                "tightened lower bound new value",
                5.0,
                t.newValue
            );
        }
    }

    checkTrue(
        testName,
        "a TightenLowerBound transformation for x was recorded",
        foundTightening
    );

    std::cout << "[PASS] " << testName << "\n";
}

// ============================================================
// No-transformation baseline
// ============================================================

void test_no_transformations_baseline() {
    const std::string testName = "test_no_transformations_baseline";

    model::Model original;
    original.name = "NoOpModel";
    original.variables.push_back(makeVariable("x", 1.0, 2.0));
    original.variables.push_back(makeVariable("y", 1.0, 2.0));

    original.constraints.push_back(
        makeConstraint("C1", 0.0, 100.0, { term(0, 1.0), term(1, 3.0) })
    );

    presolve::Presolver presolver;
    presolve::PresolveResult result = presolver.run(original);

    checkTrue(testName, "model is not infeasible", !result.infeasible);

    checkTrue(
        testName,
        "presolvedVariables equals originalVariables (no elimination expected)",
        result.presolvedVariables == result.originalVariables
    );

    checkTrue(
        testName,
        "presolvedConstraints equals originalConstraints (no elimination expected)",
        result.presolvedConstraints == result.originalConstraints
    );

    std::cout << "[PASS] " << testName << "\n";
}

int main() {
    test_fixed_variable_recorded();
    test_singleton_equality_positive_coefficient();
    test_singleton_equality_negative_coefficient();
    test_bound_tightening_recorded();
    test_no_transformations_baseline();

    std::cout << "All presolve metadata tests passed successfully!" << std::endl;
    return 0;
}