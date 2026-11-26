#pragma once

#include "paper_parallel_case.hpp"

#include <string>

namespace dmf::paper {

struct TrialSettings {
    Point output_port;
    int mix_cycles = 3;
    int detect_cycles = 2;
    std::optional<std::map<std::string, Point>> inlets = std::nullopt;
};

struct CalibrationCase {
    std::string name;
    ParallelCase chip;
    TrialSettings settings;
    std::string branches;
};

std::vector<CalibrationCase> loadCalibrationCases(const std::string& path);

}  // namespace dmf::paper
