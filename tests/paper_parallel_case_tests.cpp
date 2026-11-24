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

void nineGroupsHaveValidInputsAndDistinctOutputs() {
    const auto protocol = dmf::paper::makeProtocol();
    protocol.validate();
    dmf::DynamicDag dag(protocol);
    dag.activateStart({});
    check(dag.operationCount() == 36 &&
              protocol.block("start").decisions.size() == 9,
          "nine-mix case has wrong operation or decision count");

    const auto outcomes = dmf::paper::outcomesFor(100, 1400);
    check(outcomes.size() == 9 &&
              std::all_of(outcomes.begin(), outcomes.end(),
                          [](const auto& value) { return value == "output"; }),
          "deterministic scenario produced a waste branch");
    const auto feedback = dmf::paper::feedbackFor(outcomes);
    check(feedback.size() == 36, "completion feedback is missing operations");
    std::vector<int> ids;
    for (const auto& [name, value] : feedback) {
        (void)name;
        ids.insert(ids.end(), value.outputs.begin(), value.outputs.end());
    }
    std::sort(ids.begin(), ids.end());
    check(ids.size() == 36 && ids.front() == 1 && ids.back() == 36 &&
              std::adjacent_find(ids.begin(), ids.end()) == ids.end(),
          "two operations generate the same droplet id");
}

void sourcePortsStayOnBoundaryAndSeparated() {
    const auto ports = dmf::paper::sourcePorts();
    check(ports.size() == 18, "nine groups need eighteen source ports");
    for (auto it = ports.begin(); it != ports.end(); ++it) {
        const auto p = it->second;
        check(p.x >= 0 && p.x < 30 && p.y >= 0 && p.y < 30 &&
                  (p.x == 0 || p.x == 29 || p.y == 0 || p.y == 29),
              "source port is off the 30x30 boundary");
        for (auto other = std::next(it); other != ports.end(); ++other) {
            const auto q = other->second;
            check(std::max(std::abs(p.x - q.x), std::abs(p.y - q.y)) >= 2,
                  "source ports violate droplet separation");
        }
    }
}

}  // namespace

int main() {
    nineGroupsHaveValidInputsAndDistinctOutputs();
    sourcePortsStayOnBoundaryAndSeparated();
}
