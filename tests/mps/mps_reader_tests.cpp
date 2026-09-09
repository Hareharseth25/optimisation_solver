#include <iostream>
#include <cassert>
#include <cmath>
#include <stdexcept>
#include "mps/mps_reader.h"

void test_basic_lp() {
    mps::MpsReader reader;
    model::Model m = reader.read("tests/mps/test_cases/01_basic_lp.mps");
    assert(m.variables.size() == 2);
    assert(m.constraints.size() == 3);

    // Objective linear terms checking
    assert(m.objective.linearTerms.size() == 2);

    // Constraint senses check (L, G, E)
    // L: [-inf, 10.0]
    assert(std::isinf(m.constraints[0].lowerBound) && m.constraints[0].lowerBound < 0);
    assert(m.constraints[0].upperBound == 10.0);

    // G: [5.0, +inf]
    assert(m.constraints[1].lowerBound == 5.0);
    assert(std::isinf(m.constraints[1].upperBound) && m.constraints[1].upperBound > 0);

    // E: [7.0, 7.0]
    assert(m.constraints[2].lowerBound == 7.0);
    assert(m.constraints[2].upperBound == 7.0);
    std::cout << "[PASS] Test 1: Basic LP (L, G, E)" << std::endl;
}

void test_multi_column_coeffs() {
    mps::MpsReader reader;
    model::Model m = reader.read("tests/mps/test_cases/02_multi_column_coeffs.mps");

    assert(m.variables.size() == 2);
    assert(m.constraints.size() == 2);

    // Check multiple coefficients on single COLUMNS line
    assert(m.constraints[0].linearTerms.size() == 1);
    assert(m.constraints[0].linearTerms[0].value == 2.5);
    std::cout << "[PASS] Test 2: Multi-column Coefficients" << std::endl;
}

void test_all_bound_types() {
    mps::MpsReader reader;
    model::Model m = reader.read("tests/mps/test_cases/03_all_bound_types.mps");

    // LO: [2.5, +inf]
    assert(m.variables[0].lowerBound == 2.5);
    assert(std::isinf(m.variables[0].upperBound));

    // UP: [0.0, 10.0] (default LO=0)
    assert(m.variables[1].lowerBound == 0.0);
    assert(m.variables[1].upperBound == 10.0);

    // FX: [7.0, 7.0]
    assert(m.variables[2].lowerBound == 7.0);
    assert(m.variables[2].upperBound == 7.0);

    // FR: [-inf, +inf]
    assert(std::isinf(m.variables[3].lowerBound) && m.variables[3].lowerBound < 0);
    assert(std::isinf(m.variables[3].upperBound) && m.variables[3].upperBound > 0);
    std::cout << "[PASS] Test 3: All Bound Types (LO, UP, FX, FR)" << std::endl;
}

void test_bounds_only_variable() {
    mps::MpsReader reader;
    model::Model m = reader.read("tests/mps/test_cases/04_bounds_only_variable.mps");

    // X_UNSEEN appeared only in BOUNDS section
    assert(m.variables.size() == 2);
    bool found_unseen = false;
    for (const auto& var : m.variables) {
        if (var.name == "X_UNSEEN") {
            found_unseen = true;
            assert(var.lowerBound == 3.0);
            assert(var.upperBound == 8.0);
        }
    }
    assert(found_unseen);
    std::cout << "[PASS] Test 4: Variable appearing only in BOUNDS" << std::endl;
}

void test_negatives_comments_blanks() {
    mps::MpsReader reader;
    model::Model m = reader.read("tests/mps/test_cases/05_negatives_comments_blanks.mps");

    assert(m.name == "NEG_COMMENTS");
    assert(m.objective.linearTerms.size() == 2);

    // Find the objective term for variable X1
    bool found_x1 = false;
    for (const auto& term : m.objective.linearTerms) {
        // If your linearTerm uses varIndex, check against m.variables[term.varIndex].name
        // Or if it stores varName directly, check term.varName == "X1"
        if (m.variables[term.variableIndex].name == "X1") {
            assert(std::abs(term.value - (-5.25)) < 1e-6);
            found_x1 = true;
        }
    }
    assert(found_x1);

    // Check constraint RHS negative value
    assert(m.constraints[0].upperBound == -15.0);
    std::cout << "[PASS] Test 5: Negative coefficients, comments, and blank lines" << std::endl;
}

void test_malformed_missing_section() {
    mps::MpsReader reader;
    // Should parse without crashing, returning an empty/partial model IR safely
    model::Model m = reader.read("tests/mps/test_cases/06_malformed_missing_section.mps");
    assert(m.name == "MALFORMED");
    assert(m.constraints.empty());
    std::cout << "[PASS] Test 6: Malformed / missing section fallback" << std::endl;
}

int main() {
    std::cout << "--- Running MPS Reader Test Suite ---" << std::endl;
    test_basic_lp();
    test_multi_column_coeffs();
    test_all_bound_types();
    test_bounds_only_variable();
    test_negatives_comments_blanks();
    test_malformed_missing_section();
    std::cout << "All MPS parser tests completed successfully!" << std::endl;
    return 0;
}