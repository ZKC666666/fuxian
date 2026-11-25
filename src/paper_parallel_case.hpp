#pragma once

#include "dmf/execution_loop.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace dmf::paper {

constexpr int group_count = 9;

struct ParallelCase {
    int chip_side;
    int modules_per_side;
    int groups;

    Point outputPort() const { return {chip_side - 1, 0}; }
    Point wastePort() const { return {0, chip_side - 1}; }
};

std::optional<ParallelCase> caseForGroups(int groups);
std::string sourceName(int group, char side);
std::string mixName(int group);
std::string detectName(int group);
Protocol makeProtocol(const ParallelCase& config);
std::map<std::string, Point> sourcePorts(const ParallelCase& config);
std::vector<std::string> outcomesFor(const ParallelCase& config,
                                     int output_percent, unsigned seed);
std::map<std::string, CompletionFeedback> feedbackFor(
    const std::vector<std::string>& outcomes);

}  // namespace dmf::paper
