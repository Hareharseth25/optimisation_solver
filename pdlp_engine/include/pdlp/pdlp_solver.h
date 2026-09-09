#pragma once

#include "pdlp/compiled_lp.h"
#include "pdlp/pdlp_options.h"
#include "pdlp/pdlp_result.h"

namespace pdlp {

class PdlpSolver {
public:
    [[nodiscard]] PdlpResult solve(
        const CompiledLp& problem,
        const PdlpOptions& options = {}
    ) const;
};

}  // namespace pdlp

