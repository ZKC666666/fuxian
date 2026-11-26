#include "paper_trial_config.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace dmf::paper {
namespace {

void validatePorts(const ParallelCase& chip, const TrialSettings& settings) {
    const int edge = chip.chip_side - 1;
    const auto on_boundary = [edge](Point p) {
        return p.x >= 0 && p.x <= edge && p.y >= 0 && p.y <= edge &&
               (p.x == 0 || p.x == edge || p.y == 0 || p.y == edge);
    };
    std::vector<Point> placed{chip.wastePort()};
    const auto add_port = [&](Point p) {
        if (!on_boundary(p)) throw std::invalid_argument("port is outside the chip boundary");
        for (const auto q : placed) {
            if (std::max(std::abs(p.x - q.x), std::abs(p.y - q.y)) < 2) {
                throw std::invalid_argument("ports overlap or violate the minimum separation");
            }
        }
        placed.push_back(p);
    };
    add_port(settings.output_port);
    const auto ports = settings.inlets ? *settings.inlets : sourcePorts(chip);
    for (const auto& port : ports) add_port(port.second);
}

CalibrationCase readCase(std::istringstream& row, std::string name) {
    int groups = 0;
    TrialSettings settings{};
    std::string branches;
    if (!(row >> groups >> settings.output_port.x >> settings.output_port.y >>
          settings.mix_cycles >> settings.detect_cycles >> branches)) {
        throw std::invalid_argument("expected groups, outlet, durations and branch pattern");
    }
    const auto chip = caseForGroups(groups);
    if (!chip || branches.size() != static_cast<std::size_t>(groups) ||
        branches.find_first_not_of("OW") != std::string::npos) {
        throw std::invalid_argument("unsupported group count or invalid O/W branch pattern");
    }
    if (settings.mix_cycles < 1 || settings.mix_cycles > 200 ||
        settings.detect_cycles < 1 || settings.detect_cycles > 200) {
        throw std::invalid_argument("Mix and Detect timings must be between 1 and 200");
    }
    row >> std::ws;
    if (!row.eof()) {
        settings.inlets.emplace();
        for (int group = 0; group < groups; ++group) {
            for (const char side : {'a', 'b'}) {
                Point inlet{};
                if (!(row >> inlet.x >> inlet.y)) {
                    throw std::invalid_argument("incomplete inlet coordinate list");
                }
                settings.inlets->emplace(sourceName(group, side), inlet);
            }
        }
        row >> std::ws;
        if (!row.eof()) throw std::invalid_argument("extra fields after the inlet coordinates");
    }
    validatePorts(*chip, settings);
    return {std::move(name), *chip, std::move(settings), std::move(branches)};
}

}  // namespace

std::vector<CalibrationCase> loadCalibrationCases(const std::string& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open " + path);

    std::set<std::string> names;
    std::vector<CalibrationCase> cases;
    std::string line;
    int line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        std::istringstream row(line);
        std::string name;
        if (!(row >> name) || name.front() == '#') continue;
        try {
            if (name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")
                    != std::string::npos || !names.insert(name).second) {
                throw std::invalid_argument("invalid or duplicate case name");
            }
            cases.push_back(readCase(row, std::move(name)));
        } catch (const std::invalid_argument& error) {
            throw std::invalid_argument(path + ":" + std::to_string(line_number) + ": " + error.what());
        }
    }
    if (cases.empty()) throw std::invalid_argument("calibration file has no cases");
    return cases;
}

}  // namespace dmf::paper
