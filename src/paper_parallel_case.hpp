#pragma once

#include "dmf/execution_loop.hpp"

#include <map>
#include <string>
#include <vector>

namespace dmf::paper {

constexpr int group_count = 9;

std::string sourceName(int group, char side);
std::string mixName(int group);
std::string detectName(int group);
Protocol makeProtocol();
std::map<std::string, Point> sourcePorts();
std::vector<std::string> outcomesFor(int output_percent, unsigned seed);
std::map<std::string, CompletionFeedback> feedbackFor(
    const std::vector<std::string>& outcomes);

}  // namespace dmf::paper
