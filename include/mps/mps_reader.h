#ifndef MPS_MPS_READER_H_
#define MPS_MPS_READER_H_

#include <string>
#include <unordered_map>
#include <vector>
#include <limits>
#include "model/model.h"

namespace mps {

enum class MpsSection {
    NONE,
    NAME,
    ROWS,
    COLUMNS,
    RHS,
    BOUNDS,
    ENDATA
};

// Internal tracker for Row Senses during parsing
enum class RowSense {
    LESS_EQUAL,    // L
    GREATER_EQUAL, // G
    EQUAL          // E
};

class MpsReader {
public:
    MpsReader() = default;

    // Parses a basic LP MPS file and returns a populated model::Model object
    model::Model read(const std::string& filepath);

private:
    MpsSection current_section_ = MpsSection::NONE;
    std::string objective_row_name_;
    
    // Fast name-to-index mappings
    std::unordered_map<std::string, size_t> var_name_to_idx_;
    std::unordered_map<std::string, size_t> constraint_name_to_idx_;

    // Internal lookup for Row Senses (explicitly tracked per constraint)
    std::unordered_map<std::string, RowSense> row_senses_;

    // Internal linear term accumulators (Variable Index -> Coefficient)
    std::unordered_map<int, double> obj_term_map_;
    std::vector<std::unordered_map<int, double>> constraint_term_maps_;

    // Internal helpers
    int get_or_create_variable(const std::string& var_name, model::Model& model);
    void add_coefficient(const std::string& var_name, const std::string& row_name, double value, model::Model& model);
};

} // namespace mps

#endif // MPS_MPS_READER_H_