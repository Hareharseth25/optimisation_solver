#pragma once

#include "pdlp/sparse_matrix.h"

#include <cstddef>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace pdlp {

// Internal minimization form:
//   minimize objective^T x + objectiveOffset
//   subject to rowLower <= A x <= rowUpper
//              variableLower <= x <= variableUpper
struct CompiledLp {
    SparseMatrix matrix;

    std::vector<double> objective;
    double objectiveOffset = 0.0;

    std::vector<double> rowLower;
    std::vector<double> rowUpper;

    std::vector<double> variableLower;
    std::vector<double> variableUpper;

    [[nodiscard]] int numRows() const noexcept { return matrix.rows(); }
    [[nodiscard]] int numColumns() const noexcept { return matrix.columns(); }

    void validate() const {
        if (!matrix.validate()) {
            throw std::invalid_argument("CompiledLp contains an invalid sparse matrix");
        }
        if (objective.size() != static_cast<std::size_t>(numColumns()) ||
            variableLower.size() != static_cast<std::size_t>(numColumns()) ||
            variableUpper.size() != static_cast<std::size_t>(numColumns())) {
            throw std::invalid_argument("CompiledLp column vector dimensions do not match A");
        }
        if (rowLower.size() != static_cast<std::size_t>(numRows()) ||
            rowUpper.size() != static_cast<std::size_t>(numRows())) {
            throw std::invalid_argument("CompiledLp row vector dimensions do not match A");
        }
        if (!std::isfinite(objectiveOffset)) {
            throw std::invalid_argument("Objective offset must be finite");
        }

        for (std::size_t j = 0; j < objective.size(); ++j) {
            if (!std::isfinite(objective[j]) ||
                std::isnan(variableLower[j]) || std::isnan(variableUpper[j]) ||
                variableLower[j] > variableUpper[j]) {
                throw std::invalid_argument("Invalid objective coefficient or variable bounds");
            }
        }
        for (std::size_t i = 0; i < rowLower.size(); ++i) {
            if (std::isnan(rowLower[i]) || std::isnan(rowUpper[i]) ||
                rowLower[i] > rowUpper[i]) {
                throw std::invalid_argument("Invalid row bounds");
            }
        }
    }
};

}  // namespace pdlp

