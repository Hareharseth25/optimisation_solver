#include "mps/mps_reader.h"
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace mps {

model::Model MpsReader::read(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        throw std::runtime_error("MpsReader Error: Unable to open file " + filepath);
    }

    model::Model model;
    std::string line;

    // Reset internal state
    current_section_ = MpsSection::NONE;
    objective_row_name_.clear();
    var_name_to_idx_.clear();
    constraint_name_to_idx_.clear();
    row_senses_.clear();
    obj_term_map_.clear();
    constraint_term_maps_.clear();

    // Default Phase 1 objective sense (Minimization)
    model.objective.sense = model::ObjectiveSense::Minimize;

    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '*') continue;

        std::stringstream ss(line);
        std::string token;
        ss >> token;

        // Check for Header Section indicators (Column 1)
        if (line[0] != ' ' && line[0] != '\t') {
            if (token == "NAME") {
                current_section_ = MpsSection::NAME;
                if (ss >> token) {
                    model.name = token;
                }
            } else if (token == "ROWS") {
                current_section_ = MpsSection::ROWS;
            } else if (token == "COLUMNS") {
                current_section_ = MpsSection::COLUMNS;
            } else if (token == "RHS") {
                current_section_ = MpsSection::RHS;
            } else if (token == "BOUNDS") {
                current_section_ = MpsSection::BOUNDS;
            } else if (token == "ENDATA") {
                current_section_ = MpsSection::ENDATA;
                break;
            }
            continue;
        }

        // Process Data Lines inside Sections
        switch (current_section_) {
            case MpsSection::ROWS: {
                std::string sense = token;
                std::string row_name;
                ss >> row_name;

                if (sense == "N") {
                    if (objective_row_name_.empty()) {
                        objective_row_name_ = row_name;
                    }
                } else {
                    model::Constraint constraint;
                    constraint.name = row_name;

                    // Initial default bounds before RHS processing
                    if (sense == "L") {
                        constraint.lowerBound = -std::numeric_limits<double>::infinity();
                        constraint.upperBound = 0.0;
                        row_senses_[row_name] = RowSense::LESS_EQUAL;
                    } else if (sense == "G") {
                        constraint.lowerBound = 0.0;
                        constraint.upperBound = std::numeric_limits<double>::infinity();
                        row_senses_[row_name] = RowSense::GREATER_EQUAL;
                    } else if (sense == "E") {
                        constraint.lowerBound = 0.0;
                        constraint.upperBound = 0.0;
                        row_senses_[row_name] = RowSense::EQUAL;
                    }

                    size_t c_idx = model.constraints.size();
                    model.constraints.push_back(constraint);
                    constraint_name_to_idx_[row_name] = c_idx;
                    constraint_term_maps_.emplace_back();
                }
                break;
            }

            case MpsSection::COLUMNS: {
                std::string var_name = token;
                std::string row_name1;
                double val1;

                if (ss >> row_name1 >> val1) {
                    add_coefficient(var_name, row_name1, val1, model);
                }

                std::string row_name2;
                double val2;
                if (ss >> row_name2 >> val2) {
                    add_coefficient(var_name, row_name2, val2, model);
                }
                break;
            }

            case MpsSection::RHS: {
                std::string rhs_label = token;
                std::string row_name;
                double val;

                while (ss >> row_name >> val) {
                    auto it = constraint_name_to_idx_.find(row_name);
                    if (it != constraint_name_to_idx_.end()) {
                        auto& c = model.constraints[it->second];
                        RowSense sense = row_senses_[row_name];

                        // Set bounds based on explicitly tracked RowSense enum
                        if (sense == RowSense::LESS_EQUAL) {
                            c.upperBound = val;
                        } else if (sense == RowSense::GREATER_EQUAL) {
                            c.lowerBound = val;
                        } else if (sense == RowSense::EQUAL) {
                            c.lowerBound = val;
                            c.upperBound = val;
                        }
                    }
                }
                break;
            }

            case MpsSection::BOUNDS: {
                std::string bound_type = token;
                std::string bound_label, var_name;
                ss >> bound_label >> var_name;

                int v_idx = get_or_create_variable(var_name, model);
                auto& var = model.variables[v_idx];
                double val = 0.0;

                if (bound_type == "LO") {
                    ss >> val;
                    var.lowerBound = val;
                } else if (bound_type == "UP") {
                    ss >> val;
                    var.upperBound = val;
                } else if (bound_type == "FX") {
                    ss >> val;
                    var.lowerBound = val;
                    var.upperBound = val;
                } else if (bound_type == "FR") {
                    var.lowerBound = -std::numeric_limits<double>::infinity();
                    var.upperBound = std::numeric_limits<double>::infinity();
                }
                break;
            }

            default:
                break;
        }
    }

    // Convert internal accumulated maps into std::vector<model::LinearTerm>
    for (const auto& pair : obj_term_map_) {
        model.objective.linearTerms.push_back(model::LinearTerm{pair.first, pair.second});
    }

    for (size_t i = 0; i < model.constraints.size(); ++i) {
        for (const auto& pair : constraint_term_maps_[i]) {
            model.constraints[i].linearTerms.push_back(model::LinearTerm{pair.first, pair.second});
        }
    }

    return model;
}

int MpsReader::get_or_create_variable(const std::string& var_name, model::Model& model) {
    auto it = var_name_to_idx_.find(var_name);
    if (it != var_name_to_idx_.end()) {
        return static_cast<int>(it->second);
    }

    int new_idx = static_cast<int>(model.variables.size());
    model::Variable var;
    var.name = var_name;
    var.type = model::VariableType::Continuous;
    var.lowerBound = 0.0;
    var.upperBound = std::numeric_limits<double>::infinity();

    model.variables.push_back(var);
    var_name_to_idx_[var_name] = new_idx;
    return new_idx;
}

void MpsReader::add_coefficient(const std::string& var_name, const std::string& row_name, double value, model::Model& model) {
    int v_idx = get_or_create_variable(var_name, model);

    if (row_name == objective_row_name_) {
        obj_term_map_[v_idx] += value;
    } else {
        auto it = constraint_name_to_idx_.find(row_name);
        if (it != constraint_name_to_idx_.end()) {
            size_t c_idx = it->second;
            constraint_term_maps_[c_idx][v_idx] += value;
        }
    }
}

} // namespace mps