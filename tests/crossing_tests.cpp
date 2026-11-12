#include "dmf/execution_loop.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

dmf::Protocol crossingProtocol() {
    dmf::BlockSpec block{
        "start", 2,
        {{"detect", dmf::OperationType::Detect,
          {dmf::InputRef::entry(0)}, 1},
         {"send", dmf::OperationType::Output,
          {dmf::InputRef::entry(1)}, 0}},
        {}};
    return {"start", {{"start", std::move(block)}}};
}

std::vector<dmf::VirtualModule> corridorWalls() {
    return {{10, {3, 0}, 3}, {11, {3, 6}, 3}};
}

void dropletCrossesBeforeOperationStarts() {
    dmf::DynamicDag dag(crossingProtocol());
    const auto block = dag.activateStart({1, 2});
    const auto detect = dag.operationFor(block, "detect");
    dmf::DropletPositions positions{{1, {7, 3}}, {2, {1, 4}}};
    dmf::VirtualChip chip(1, 1);
    dmf::UpperLayer upper(dag, chip,
                          {{dmf::OperationType::Detect, {3, 3, 2}}}, positions,
                          {{dmf::OperationType::Output, {8, 4}}});
    dmf::Router router(chip.width(), chip.height(), positions);
    dmf::ExecutionLoop loop(
        dag, upper, router,
        [detect](int operation) {
            return dmf::CompletionFeedback{
                operation == detect ? std::vector<int>{3} : std::vector<int>{},
                std::nullopt};
        }, {}, corridorWalls());

    bool entered_future_module = false;
    bool crossing_reported = false;
    bool started = false;
    for (int step = 0; step < 50 && !loop.allDone(); ++step) {
        const auto report = loop.step();
        crossing_reported |= report.crossing_modules.count(2) != 0;
        if (report.crossing_modules.count(2) != 0) {
            check(report.crossing_modules.at(2) == 1 &&
                      std::find(report.held_modules.begin(),
                                report.held_modules.end(), 1) !=
                          report.held_modules.end(),
                  "crossing did not hold the future operation module");
        }
        for (const auto& move : report.moves) {
            if (move.droplet == 2 && move.to.x >= 3 && move.to.x <= 5 &&
                move.to.y >= 3 && move.to.y <= 5) {
                entered_future_module = true;
            }
        }
        if (std::find(report.started.begin(), report.started.end(), detect) !=
            report.started.end()) {
            started = true;
            check(positions.count(2) == 0 || positions.at(2).x >= 6,
                  "operation started before the droplet reached the target side");
        }
    }
    check(crossing_reported && entered_future_module && started && loop.allDone(),
          "safe pre-activation crossing did not finish the protocol");
}

void occupiedCorridorKeepsOperationWaiting() {
    dmf::DynamicDag dag(crossingProtocol());
    const auto block = dag.activateStart({1, 2});
    const auto detect = dag.operationFor(block, "detect");
    dmf::DropletPositions positions{{1, {4, 4}}, {2, {1, 4}}};
    dmf::VirtualChip chip(1, 1);
    dmf::UpperLayer upper(dag, chip,
                          {{dmf::OperationType::Detect, {3, 3, 2}}}, positions,
                          {{dmf::OperationType::Output, {8, 4}}});
    dmf::Router router(chip.width(), chip.height(), positions);
    dmf::ExecutionLoop loop(
        dag, upper, router,
        [detect](int operation) {
            return dmf::CompletionFeedback{
                operation == detect ? std::vector<int>{3} : std::vector<int>{},
                std::nullopt};
        }, {}, corridorWalls());

    for (int step = 0; step < 8; ++step) {
        const auto report = loop.step();
        check(report.crossing_modules.count(2) != 0 &&
                  std::find(report.waiting_for_crossing.begin(),
                            report.waiting_for_crossing.end(), 2) !=
                      report.waiting_for_crossing.end() &&
                  report.active_blockages == 0 &&
                  dag.operation(detect).state == dmf::OperationState::Placed,
              "occupied corridor activated its future blockage");
    }
}

void failedCrossingMoveRetriesFromObservedPosition() {
    dmf::DynamicDag dag(crossingProtocol());
    const auto block = dag.activateStart({1, 2});
    const auto detect = dag.operationFor(block, "detect");
    dmf::DropletPositions positions{{1, {7, 3}}, {2, {1, 4}}};
    dmf::VirtualChip chip(1, 1);
    dmf::UpperLayer upper(dag, chip,
                          {{dmf::OperationType::Detect, {3, 3, 2}}}, positions,
                          {{dmf::OperationType::Output, {8, 4}}});
    dmf::Router router(chip.width(), chip.height(), positions);
    dmf::ExecutionLoop loop(
        dag, upper, router,
        [detect](int operation) {
            return dmf::CompletionFeedback{
                operation == detect ? std::vector<int>{3} : std::vector<int>{},
                std::nullopt};
        },
        [](const dmf::MoveCommand& move, int step) {
            return step == 1 && move.droplet == 2 ? move.from : move.to;
        }, corridorWalls());

    const auto failed = loop.step();
    check(failed.crossing_modules.count(2) != 0 &&
              positions.at(2) == dmf::Point{1, 4},
          "failed crossing move changed the observed position");
    const auto retried = loop.step();
    const auto move = std::find_if(retried.moves.begin(), retried.moves.end(),
                                   [](const auto& entry) { return entry.droplet == 2; });
    check(move != retried.moves.end() && move->from == dmf::Point{1, 4} &&
              move->to == dmf::Point{2, 4},
          "crossing did not retry from the observed position");
    for (int step = 0; step < 50 && !loop.allDone(); ++step) loop.step();
    check(loop.allDone(), "protocol did not finish after crossing retry");
}

}  // namespace

int main() {
    try {
        dropletCrossesBeforeOperationStarts();
        occupiedCorridorKeepsOperationWaiting();
        failedCrossingMoveRetriesFromObservedPosition();
        std::cout << "crossing tests: passed\n";
    } catch (const std::exception& error) {
        std::cerr << "crossing tests: " << error.what() << '\n';
        return 1;
    }
}
