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

bool contains(const std::vector<int>& values, int value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

dmf::Protocol oneDetect() {
    dmf::BlockSpec block{
        "start", 1,
        {{"detect", dmf::OperationType::Detect,
          {dmf::InputRef::entry(0)}, 1}},
        std::nullopt};
    return {"start", {{"start", std::move(block)}}};
}

dmf::Protocol twoDetects() {
    dmf::BlockSpec block{
        "start", 2,
        {{"near", dmf::OperationType::Detect,
          {dmf::InputRef::entry(0)}, 1},
         {"far", dmf::OperationType::Detect,
          {dmf::InputRef::entry(1)}, 1}},
        std::nullopt};
    return {"start", {{"start", std::move(block)}}};
}

void idleDropletEvacuatesBeforeStart() {
    dmf::DynamicDag dag(oneDetect());
    const auto block = dag.activateStart({1});
    const auto detect = dag.operationFor(block, "detect");
    dmf::DropletPositions positions{{1, {4, 3}}, {99, {4, 5}}};
    dmf::VirtualChip chip(1, 1);
    dmf::UpperLayer upper(dag, chip,
                          {{dmf::OperationType::Detect, {3, 3, 2}}}, positions);
    dmf::Router router(chip.width(), chip.height(), positions);
    dmf::ExecutionLoop loop(
        dag, upper, router,
        [](int) { return dmf::CompletionFeedback{{2}, std::nullopt}; });

    const auto first = loop.step();
    check(first.evacuation_targets.count(99) != 0 &&
              first.evacuation_targets.at(99) == dmf::Point{4, 6} &&
              contains(first.evacuated, 99),
          "idle droplet did not evacuate to the nearest safe outside cell");
    check(first.started.empty() && first.active_blockages == 0 &&
              dag.operation(detect).state == dmf::OperationState::Placed,
          "operation started before its reserved module was clear");

    bool started = false;
    for (int step = 0; step < 12 && !loop.allDone(); ++step) {
        const auto report = loop.step();
        if (contains(report.started, detect)) {
            started = true;
            check(positions.at(99) == dmf::Point{4, 6} &&
                      report.active_blockages == 1,
                  "operation started while the unrelated droplet was inside");
        }
    }
    check(started && loop.allDone(), "operation did not start after evacuation");
}

void movingDropletResumesItsOriginalTask() {
    dmf::DynamicDag dag(twoDetects());
    const auto block = dag.activateStart({1, 2});
    const auto near = dag.operationFor(block, "near");
    const auto far = dag.operationFor(block, "far");
    dmf::DropletPositions positions{{1, {4, 3}}, {2, {5, 5}}};
    dmf::VirtualChip chip(2, 1);
    dmf::UpperLayer upper(dag, chip,
                          {{dmf::OperationType::Detect, {3, 3, 3}}}, positions);
    dmf::Router router(chip.width(), chip.height(), positions);
    dmf::ExecutionLoop loop(
        dag, upper, router,
        [=](int operation) {
            return dmf::CompletionFeedback{{operation == near ? 3 : 4},
                                           std::nullopt};
        });

    const auto first = loop.step();
    check(first.routing_tasks.size() == 2 &&
              first.evacuation_targets.count(2) != 0 &&
              contains(first.evacuated, 2) &&
              dag.operation(far).state == dmf::OperationState::Placed,
          "moving droplet did not temporarily evacuate");
    const auto exit = positions.at(2);
    check(exit.x < 3 || exit.x > 5 || exit.y < 3 || exit.y > 5,
          "moving droplet remained in the future blockage");
    bool resumed = false;
    bool far_started = false;
    for (int step = 0; step < 40 && !loop.allDone(); ++step) {
        const auto report = loop.step();
        for (const auto& move : report.moves) {
            if (move.droplet == 2 && move.from == exit &&
                !(move.to == exit)) resumed = true;
        }
        if (contains(report.started, far)) far_started = true;
    }
    check(resumed && far_started && loop.allDone(),
          "original routing task did not resume after evacuation");
}

void failedEvacuationMoveWaitsForObservedPosition() {
    dmf::DynamicDag dag(oneDetect());
    const auto block = dag.activateStart({1});
    const auto detect = dag.operationFor(block, "detect");
    dmf::DropletPositions positions{{1, {4, 3}}, {99, {4, 5}}};
    dmf::VirtualChip chip(1, 1);
    dmf::UpperLayer upper(dag, chip,
                          {{dmf::OperationType::Detect, {3, 3, 2}}}, positions);
    dmf::Router router(chip.width(), chip.height(), positions);
    dmf::ExecutionLoop loop(
        dag, upper, router,
        [](int) { return dmf::CompletionFeedback{{2}, std::nullopt}; },
        [](const dmf::MoveCommand& move, int step) {
            return step == 1 && move.droplet == 99 ? move.from : move.to;
        });

    const auto failed = loop.step();
    check(positions.at(99) == dmf::Point{4, 5} && failed.evacuated.empty() &&
              failed.started.empty() && failed.active_blockages == 0,
          "failed evacuation was treated as successful");
    const auto retried = loop.step();
    check(contains(retried.evacuated, 99) &&
              positions.at(99) == dmf::Point{4, 6},
          "evacuation did not retry from the observed position");
    for (int step = 0; step < 12 && !loop.allDone(); ++step) loop.step();
    check(loop.allDone() && dag.operation(detect).state ==
                                dmf::OperationState::Completed,
          "operation did not finish after retrying evacuation");
}

void noSafeExitReportsWaiting() {
    dmf::DynamicDag dag(oneDetect());
    const auto block = dag.activateStart({1});
    const auto detect = dag.operationFor(block, "detect");
    dmf::DropletPositions positions{{1, {0, 0}}, {99, {4, 4}},
                                    {2, {1, 4}}, {3, {7, 4}},
                                    {4, {4, 1}}, {5, {4, 7}}};
    dmf::VirtualChip chip(1, 1);
    dmf::UpperLayer upper(dag, chip,
                          {{dmf::OperationType::Detect, {3, 3, 2}}}, positions);
    dmf::Router router(chip.width(), chip.height(), positions);
    dmf::ExecutionLoop loop(
        dag, upper, router,
        [](int) { return dmf::CompletionFeedback{{6}, std::nullopt}; });

    const auto report = loop.step();
    check(contains(report.waiting_for_evacuation, 99) &&
              report.evacuation_targets.count(99) == 0 &&
              positions.at(99) == dmf::Point{4, 4},
          "unreachable evacuation did not report waiting");
    check(report.started.empty() && report.active_blockages == 0 &&
              dag.operation(detect).state == dmf::OperationState::Placed,
          "operation activated while its future module was occupied");
    const auto next = loop.step();
    check(contains(next.waiting_for_evacuation, 99) &&
              next.active_blockages == 0 && positions.at(99) == dmf::Point{4, 4},
          "an unresolved evacuation was not kept in the waiting state");
}

void futureModuleIsAvoidedBeforeActivation() {
    dmf::DropletPositions positions{{1, {0, 4}}};
    dmf::Router router(9, 9, positions);
    router.addTasks({{1, 1, {8, 4}, 0}});
    router.setUpcomingBlockages({{{1, {3, 3}, 3}, {}}});
    int steps = 0;
    while (router.hasTasks() && steps++ < 30) {
        std::vector<dmf::MoveFeedback> feedback;
        for (const auto& move : router.planStep()) {
            check(move.to.x < 3 || move.to.x > 5 ||
                      move.to.y < 3 || move.to.y > 5,
                  "unrelated droplet entered a future operation module");
            feedback.push_back({move.droplet, move.to});
        }
        router.applyFeedback(feedback);
    }
    check(!router.hasTasks() && positions.at(1) == dmf::Point{8, 4},
          "router did not detour around the future module");
}

}  // namespace

int main() {
    try {
        idleDropletEvacuatesBeforeStart();
        movingDropletResumesItsOriginalTask();
        failedEvacuationMoveWaitsForObservedPosition();
        noSafeExitReportsWaiting();
        futureModuleIsAvoidedBeforeActivation();
        std::cout << "evacuation tests: passed\n";
    } catch (const std::exception& error) {
        std::cerr << "evacuation tests: " << error.what() << '\n';
        return 1;
    }
}
