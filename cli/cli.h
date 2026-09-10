#pragma once

#include <iostream>

namespace cli {

void printBanner(std::ostream& out);

int run(int argc, char* argv[], std::ostream& out = std::cout, std::ostream& err = std::cerr);

}  // namespace cli
