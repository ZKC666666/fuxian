#pragma once

#include "dmf/experiment_metrics.hpp"

#include <string>

namespace dmf::demo {

ExperimentSummary runParallelProtocol(const std::string& first_outcome,
                                      const std::string& second_outcome,
                                      bool trace, bool print_summary = true,
                                      BranchActivationMode mode = BranchActivationMode::Immediate);

}  // namespace dmf::demo
