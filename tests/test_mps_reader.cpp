#include <iostream>
#include <cassert>
#include <fstream>
#include <cmath>
#include "mps/mps_reader.h"

void create_test_mps_file(const std::string& filename) {
    std::ofstream out(filename);
    out << "NAME          TESTLP\n"
        << "ROWS\n"
        << " N  OBJ\n"
        << " L  C_LESS\n"
        << " G  C_GREATER\n"
        << " E  C_EQUAL\n"
        << "COLUMNS\n"
        << "    X1        OBJ       3.0   C_LESS    1.0\n"
        << "    X1        C_GREATER 2.0\n"
        << "    X2        OBJ       4.0   C_LESS    2.0\n"
        << "    X2        C_EQUAL   1.0\n"
        << "RHS\n"
        << "    RHS1      C_LESS    10.0  C_GREATER 5.0\n"
        << "    RHS1      C_EQUAL   7.0\n"
        << "BOUNDS\n"
        << " LO BND1      X1        1.0\n"
        << " UP BND1      X1        5.0\n"
        << " LO BND1      X2        0.0\n"
        << " UP BND1      X2        10.0\n"
        << "ENDATA\n";
    out.close();
}

int main() {
    std::string test_filename = "temp_test_lp.mps";
    create_test_mps_file(test_filename);

    mps::MpsReader reader;
    model::Model model = reader.read(test_filename);

    // 1. Verify Model Name
    assert(model.name == "TESTLP");

    // 2. Verify Variables Count & Indexing
    assert(model.variables.size() == 2);
    assert(model.variables[0].name == "X1");
    assert(model.variables[0].lowerBound == 1.0);
    assert(model.variables[0].upperBound == 5.0);

    assert(model.variables[1].name == "X2");
    assert(model.variables[1].lowerBound == 0.0);
    assert(model.variables[1].upperBound == 10.0);

    // 3. Verify Objective Terms (X1 index 0 -> 3.0, X2 index 1 -> 4.0)
    assert(model.objective.linearTerms.size() == 2);
    for (const auto& term : model.objective.linearTerms) {
        if (term.variableIndex == 0) assert(term.value == 3.0);
        if (term.variableIndex == 1) assert(term.value == 4.0);
    }

    // 4. Verify Constraints Count & Unified Bounds
    assert(model.constraints.size() == 3);

    // C_LESS (L): [-inf, 10.0]
    assert(model.constraints[0].name == "C_LESS");
    assert(std::isinf(model.constraints[0].lowerBound) && model.constraints[0].lowerBound < 0);
    assert(model.constraints[0].upperBound == 10.0);

    // C_GREATER (G): [5.0, +inf]
    assert(model.constraints[1].name == "C_GREATER");
    assert(model.constraints[1].lowerBound == 5.0);
    assert(std::isinf(model.constraints[1].upperBound) && model.constraints[1].upperBound > 0);

    // C_EQUAL (E): [7.0, 7.0]
    assert(model.constraints[2].name == "C_EQUAL");
    assert(model.constraints[2].lowerBound == 7.0);
    assert(model.constraints[2].upperBound == 7.0);

    // 5. Verify Constraint Linear Coefficients
    // C_LESS: 1.0*X1 + 2.0*X2
    assert(model.constraints[0].linearTerms.size() == 2);
    for (const auto& term : model.constraints[0].linearTerms) {
        if (term.variableIndex == 0) assert(term.value == 1.0);
        if (term.variableIndex == 1) assert(term.value == 2.0);
    }

    // Clean up temporary test file
    std::remove(test_filename.c_str());

    std::cout << "All MPS parser verification tests passed successfully!" << std::endl;
    return 0;
}