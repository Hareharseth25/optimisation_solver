#pragma once

#include <limits>
#include <string>
#include <vector>

namespace model {

enum class VariableType { Continuous, Integer, Binary };

struct Variable {
  std::string name;

  VariableType type = VariableType::Continuous;

  double lowerBound = 0.0;
  double upperBound = std::numeric_limits<double>::infinity();
};

struct LinearTerm {
  int variableIndex;
  double value;
};

struct QuadraticTerm {
  int variableIndex1;
  int variableIndex2;
  double value;
};

enum class ObjectiveSense { Minimize, Maximize };

struct Objective {
  ObjectiveSense sense = ObjectiveSense::Minimize;

  double offset = 0.0;

  std::vector<LinearTerm> linearTerms;
  std::vector<QuadraticTerm> quadraticTerms;
};

struct Constraint {
  std::string name;

  double lowerBound = -std::numeric_limits<double>::infinity();
  double upperBound = std::numeric_limits<double>::infinity();

  std::vector<LinearTerm> linearTerms;
};

struct Model {
  std::string name;

  Objective objective;

  std::vector<Variable> variables;
  std::vector<Constraint> constraints;

  bool validate() const;
};

} // namespace model