#include "paper_parallel_case.hpp"

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void groupsHaveValidInputsAndDistinctOutputs(const dmf::paper::ParallelCase& config) {
    const auto protocol = dmf::paper::makeProtocol(config);
    protocol.validate();
    dmf::DynamicDag dag(protocol);
    dag.activateStart({});
    const auto groups = static_cast<std::size_t>(config.groups);
    check(dag.operationCount() == 4 * config.groups &&
              protocol.block("start").decisions.size() == groups,
          "parallel case has wrong operation or decision count");

    const auto outcomes = dmf::paper::outcomesFor(config, 100, 1400);
    check(outcomes.size() == groups &&
              std::all_of(outcomes.begin(), outcomes.end(),
                          [](const auto& value) { return value == "output"; }),
          "deterministic scenario produced a waste branch");
    const auto feedback = dmf::paper::feedbackFor(outcomes);
    check(feedback.size() == 4 * groups, "completion feedback is missing operations");
    std::vector<int> ids;
    for (const auto& [name, value] : feedback) {
        (void)name;
        ids.insert(ids.end(), value.outputs.begin(), value.outputs.end());
    }
    std::sort(ids.begin(), ids.end());
    check(ids.size() == 4 * groups && ids.front() == 1 && ids.back() == 4 * config.groups &&
              std::adjacent_find(ids.begin(), ids.end()) == ids.end(),
          "two operations generate the same droplet id");
}

void sourcePortsStayOnBoundaryAndSeparated(const dmf::paper::ParallelCase& config) {
    const auto ports = dmf::paper::sourcePorts(config);
    check(ports.size() == 2 * static_cast<std::size_t>(config.groups),
          "each group needs two source ports");
    const int edge = config.chip_side - 1;
    const auto separated = [](dmf::Point a, dmf::Point b) {
        return std::max(std::abs(a.x - b.x), std::abs(a.y - b.y)) >= 2;
    };
    for (auto it = ports.begin(); it != ports.end(); ++it) {
        const auto p = it->second;
        check(p.x >= 0 && p.x <= edge && p.y >= 0 && p.y <= edge &&
                  (p.x == 0 || p.x == edge || p.y == 0 || p.y == edge),
              "source port is off the chip boundary");
        check(separated(p, config.outputPort()) && separated(p, config.wastePort()),
              "source port is too close to a terminal");
        for (auto other = std::next(it); other != ports.end(); ++other) {
            const auto q = other->second;
            check(separated(p, q),
                  "source ports violate droplet separation");
        }
    }
}

}  // namespace

int main() {
    for (const int groups : {9, 15, 25}) {
        const auto config = dmf::paper::caseForGroups(groups);
        check(config && config->groups == groups, "missing paper scale");
        groupsHaveValidInputsAndDistinctOutputs(*config);
        sourcePortsStayOnBoundaryAndSeparated(*config);
        dmf::VirtualChip chip(config->modules_per_side, config->modules_per_side, 3,
                             config->chip_side, config->chip_side);
        const auto last = chip.nearestFree({{chip.width() - 1, chip.height() - 1}}, {3, 3, 1});
        const int expected_center = groups == 9 ? 22 : 52;
        check(last && last->center() == dmf::Point{expected_center, expected_center},
              "unexpected module at lower right of chip");
    }
}
