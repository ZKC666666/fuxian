#include "dmf/execution_loop.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bool contains(const std::vector<dmf::OperationInstanceId>& ids,
              dmf::OperationInstanceId id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

dmf::Protocol parallelProtocol() {
    using dmf::InputRef;
    using dmf::OperationType;
    dmf::BlockSpec block{
        "start", 2,
        {{"near", OperationType::Detect, {InputRef::entry(0)}, 1},
         {"far", OperationType::Detect, {InputRef::entry(1)}, 1}},
        std::nullopt};
    return {"start", {{"start", std::move(block)}}};
}

dmf::Protocol dependentProtocol() {
    using dmf::InputRef;
    using dmf::OperationType;
    dmf::BlockSpec block{
        "start", 1,
        {{"first", OperationType::Detect, {InputRef::entry(0)}, 1},
         {"second", OperationType::Detect, {InputRef::output("first")}, 1}},
        std::nullopt};
    return {"start", {{"start", std::move(block)}}};
}

void routingContinuesDuringOperation() {
    dmf::DynamicDag dag(parallelProtocol());
    const auto block = dag.activateStart({1, 2});
    const auto near = dag.operationFor(block, "near");
    const auto far = dag.operationFor(block, "far");
    dmf::DropletPositions positions{{1, {4, 4}}, {2, {0, 7}}};
    dmf::VirtualChip chip(2, 1);
    dmf::UpperLayer upper(dag, chip, {{dmf::OperationType::Detect, {3, 3, 3}}},
                          positions);
    dmf::Router router(chip.width(), chip.height(), positions);
    dmf::ExecutionLoop loop(dag, upper, router, [=](int operation) {
        return dmf::CompletionFeedback{{operation == near ? 3 : 4}, std::nullopt};
    });

    const auto first = loop.step();
    check(contains(first.started, near) && !contains(first.started, far),
          "near operation did not start immediately");
    check(first.active_blockages == 1 && !first.moves.empty(),
          "routing did not overlap with an active operation");
    check(dag.operation(far).state == dmf::OperationState::Placed,
          "far operation started before its droplet arrived");
    check(loop.remainingCycles(near) == 3,
          "new operation consumed its duration on the start step");
    for (const auto& move : first.moves) {
        check(!(move.to.x >= 3 && move.to.x <= 5 &&
                move.to.y >= 3 && move.to.y <= 5),
              "routing entered an active module");
    }

    const auto second = loop.step();
    check(second.active_blockages == 1 && !second.moves.empty() &&
              loop.remainingCycles(near) == 2,
          "operation timing stopped routing progress");
    loop.step();
    const auto fourth = loop.step();
    check(contains(fourth.completed, near) && !upper.hasPlacement(near),
          "operation did not release its module after three running steps");
    loop.step();  // The cleared blockage affects the next routing decision.
    router.waitForCostFieldUpdates();
    check(router.costFieldBuilds() >= 2,
          "blockage change did not rebuild a routing cost field");

    int steps = 5;
    bool far_started = false;
    while (!loop.allDone() && steps++ < 100) {
        const auto report = loop.step();
        if (contains(report.started, far)) {
            far_started = true;
            check(loop.remainingCycles(far) == 3,
                  "late arrival reduced the operation duration");
        }
    }
    check(far_started, "far operation never started after arrival");
    check(loop.allDone() && dag.operation(far).state == dmf::OperationState::Completed,
          "far operation did not finish after routing around the blocker");
}

void outputsWaitUntilTheNextStep() {
    dmf::DynamicDag dag(dependentProtocol());
    const auto block = dag.activateStart({1});
    const auto first = dag.operationFor(block, "first");
    const auto second = dag.operationFor(block, "second");
    dmf::DropletPositions positions{{1, {4, 4}}};
    dmf::VirtualChip chip(1, 1);
    dmf::UpperLayer upper(dag, chip, {{dmf::OperationType::Detect, {3, 3, 1}}},
                          positions);
    dmf::Router router(chip.width(), chip.height(), positions);
    dmf::ExecutionLoop loop(dag, upper, router, [=](int operation) {
        return dmf::CompletionFeedback{{operation == first ? 2 : 3}, std::nullopt};
    });

    const auto start = loop.step();
    check(contains(start.started, first) && start.completed.empty(),
          "operation completed on its start step");
    const auto finish = loop.step();
    check(contains(finish.completed, first) && !contains(finish.started, second),
          "dependent operation started before output became available");
    check(dag.operation(second).state == dmf::OperationState::Waiting,
          "dependent operation was prematurely marked ready");
    const auto next = loop.step();
    check(contains(next.started, second), "output was not usable on the next step");
    loop.step();
    check(loop.allDone(), "dependent protocol did not finish");
}

void earlyCompletionReleasesModule() {
    dmf::DynamicDag dag(dependentProtocol());
    const auto block = dag.activateStart({1});
    const auto first = dag.operationFor(block, "first");
    const auto second = dag.operationFor(block, "second");
    dmf::DropletPositions positions{{1, {4, 4}}};
    dmf::VirtualChip chip(1, 1);
    dmf::UpperLayer upper(dag, chip, {{dmf::OperationType::Detect, {3, 3, 5}}},
                          positions);
    dmf::Router router(chip.width(), chip.height(), positions);
    dmf::ExecutionLoop loop(dag, upper, router,
                            [](int) { return dmf::CompletionFeedback{{3}, std::nullopt}; });
    loop.step();
    check(loop.remainingCycles(first) == 5, "early completion test did not start");
    loop.completeEarly(first, {{2}, std::nullopt});
    check(!upper.hasPlacement(first) && upper.activeBlockages().empty(),
          "early completion did not release the module");
    check(dag.operation(second).state == dmf::OperationState::Waiting,
          "early output was usable in the same step");
    check(contains(loop.step().started, second),
          "early output was not usable on the next step");
}

void forcedDetourRecoversFromMovementFailure() {
    dmf::DynamicDag dag(parallelProtocol());
    const auto block = dag.activateStart({1, 2});
    const auto near = dag.operationFor(block, "near");
    const auto far = dag.operationFor(block, "far");
    dmf::DropletPositions positions{{1, {4, 4}}, {2, {0, 4}}};
    dmf::VirtualChip chip(2, 1);
    dmf::UpperLayer upper(dag, chip, {{dmf::OperationType::Detect, {3, 3, 20}}},
                          positions);
    dmf::Router router(chip.width(), chip.height(), positions);
    dmf::ExecutionLoop loop(
        dag, upper, router,
        [=](int operation) {
            return dmf::CompletionFeedback{{operation == near ? 3 : 4}, std::nullopt};
        },
        [](const dmf::MoveCommand& command, int step) {
            return step == 2 && command.droplet == 2 ? command.from : command.to;
        });

    bool detoured = false;
    bool failed_once = false;
    bool resumed = false;
    bool far_started_during_near = false;
    dmf::Point failed_position{};
    int steps = 0;
    while (!loop.allDone() && steps++ < 60) {
        const auto report = loop.step();
        check(report.moves.size() == report.observed.size(),
              "planned and observed movement counts differ");
        for (std::size_t i = 0; i < report.moves.size(); ++i) {
            const auto& command = report.moves[i];
            const auto& feedback = report.observed[i];
            check(feedback.droplet == command.droplet &&
                      positions.at(command.droplet) == feedback.observed,
                  "step report did not show the actual position");
            if (report.step == 2 && command.droplet == 2) {
                failed_once = true;
                failed_position = command.from;
                check(feedback.observed == command.from &&
                          !(command.to == command.from),
                      "injected movement failure did not keep the droplet in place");
            }
            if (report.step == 3 && command.droplet == 2) {
                resumed = command.from == failed_position;
            }
        }
        if (dag.operation(near).state == dmf::OperationState::Running) {
            const auto point = positions.at(2);
            check(!(point.x >= 3 && point.x <= 5 &&
                    point.y >= 3 && point.y <= 5),
                  "routing entered the active near module");
            if (point.y != 4) detoured = true;
        }
        if (contains(report.started, far) &&
            dag.operation(near).state == dmf::OperationState::Running) {
            far_started_during_near = true;
        }
    }
    check(failed_once && resumed && detoured,
          "failure recovery or forced detour was not observed");
    check(far_started_during_near && loop.allDone(),
          "parallel operations did not complete after the detour");
}

void failedFinalMoveDoesNotStartOperation() {
    dmf::DynamicDag dag(dependentProtocol());
    const auto block = dag.activateStart({1});
    const auto first = dag.operationFor(block, "first");
    dmf::DropletPositions positions{{1, {4, 3}}};
    dmf::VirtualChip chip(1, 1);
    dmf::UpperLayer upper(dag, chip, {{dmf::OperationType::Detect, {3, 3, 2}}},
                          positions);
    dmf::Router router(chip.width(), chip.height(), positions);
    dmf::ExecutionLoop loop(
        dag, upper, router,
        [](int) { return dmf::CompletionFeedback{{2}, std::nullopt}; },
        [](const dmf::MoveCommand& command, int step) {
            return step == 1 ? command.from : command.to;
        });

    const auto failed = loop.step();
    check(failed.started.empty() && failed.active_blockages == 0 &&
              positions.at(1) == dmf::Point{4, 3} &&
              dag.operation(first).state == dmf::OperationState::Placed,
          "failed final move was treated as arrival");
    const auto arrived = loop.step();
    check(contains(arrived.started, first) && arrived.active_blockages == 1 &&
              positions.at(1) == dmf::Point{4, 4},
          "operation did not start after observed arrival");
}

}  // namespace

int main() {
    try {
        routingContinuesDuringOperation();
        outputsWaitUntilTheNextStep();
        earlyCompletionReleasesModule();
        forcedDetourRecoversFromMovementFailure();
        failedFinalMoveDoesNotStartOperation();
        std::cout << "execution tests: passed\n";
    } catch (const std::exception& error) {
        std::cerr << "execution tests: " << error.what() << '\n';
        return 1;
    }
}
