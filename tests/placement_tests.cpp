#include "dmf/upper_layer.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename ExpectedError, typename Action>
void rejects(Action action, const std::string& message) {
    try {
        action();
    } catch (const ExpectedError&) {
        return;
    }
    throw std::runtime_error(message);
}

dmf::Protocol mixThenDetect() {
    using dmf::InputRef;
    using dmf::OperationType;
    dmf::BlockSpec block{
        "start", 2,
        {{"mix", OperationType::Mix, {InputRef::entry(0), InputRef::entry(1)}, 1},
         {"detect", OperationType::Detect, {InputRef::output("mix")}, 1}},
        std::nullopt};
    return {"start", {{"start", std::move(block)}}};
}

void operationWaitsForBothArrivals() {
    dmf::DynamicDag dag(mixThenDetect());
    const auto block = dag.activateStart({1, 2});
    const auto mix = dag.operationFor(block, "mix");
    dmf::UpperLayer upper(dag, dmf::VirtualChip(2, 1),
                          {{dmf::OperationType::Mix, {3, 3}},
                           {dmf::OperationType::Detect, {3, 3}}},
                          {{1, {0, 4}}, {2, {2, 4}}});

    const auto tasks = upper.placeReadyOperations();
    check(tasks.size() == 2 && upper.moduleFor(mix).id == 1,
          "mix was not placed in the nearest module");
    check(tasks[0].target == dmf::Point{3, 4} && tasks[1].target == dmf::Point{5, 4},
          "mix routing targets are wrong");
    check(dag.operation(mix).state == dmf::OperationState::Placed &&
              upper.activeBlockages().empty(),
          "placement was treated as active execution");
    rejects<std::logic_error>([&] { upper.finishOperation(mix, {3}); },
                              "operation finished before its droplets arrived");
    check(!upper.reportArrival(mix, 1) && !upper.reportArrival(mix, 1),
          "one droplet or duplicate feedback started the operation");
    check(dag.operation(mix).state == dmf::OperationState::Placed,
          "operation started after only one arrival");
    check(upper.reportArrival(mix, 2) &&
              dag.operation(mix).state == dmf::OperationState::Running &&
              upper.activeBlockages().size() == 1,
          "operation did not start or block its module after both arrivals");

    upper.finishOperation(mix, {3});
    check(upper.activeBlockages().empty() && !upper.hasPlacement(mix) &&
              upper.positionOf(3) == dmf::Point{4, 4},
          "module or output position was not updated after completion");
    dag.advanceCycle();
    const auto detect = dag.operationFor(block, "detect");
    const auto detect_tasks = upper.placeReadyOperations();
    check(detect_tasks.size() == 1 && detect_tasks[0].droplet == 3 &&
              upper.moduleFor(detect).id == 1,
          "detect did not use the newly available droplet and free module");
    check(upper.reportArrival(detect, 3), "detect did not start on arrival");
    upper.finishOperation(detect, {4});
    dag.advanceCycle();
    check(dag.allDone(), "mix and detect sequence did not complete");
}

void occupiedModuleDefersAnotherOperation() {
    using dmf::InputRef;
    using dmf::OperationType;
    dmf::BlockSpec block{
        "start", 4,
        {{"first", OperationType::Mix, {InputRef::entry(0), InputRef::entry(1)}, 1},
         {"second", OperationType::Mix, {InputRef::entry(2), InputRef::entry(3)}, 1}},
        std::nullopt};
    dmf::DynamicDag dag({"start", {{"start", std::move(block)}}});
    const auto start = dag.activateStart({1, 2, 3, 4});
    const auto first = dag.operationFor(start, "first");
    const auto second = dag.operationFor(start, "second");
    dmf::UpperLayer upper(dag, dmf::VirtualChip(1, 1),
                          {{OperationType::Mix, {3, 3}}},
                          {{1, {0, 1}}, {2, {0, 3}}, {3, {1, 0}}, {4, {3, 0}}});

    check(upper.placeReadyOperations().size() == 2 && upper.hasPlacement(first) &&
              !upper.hasPlacement(second),
          "one module accepted two simultaneous reservations");
    check(dag.operation(second).state == dmf::OperationState::Waiting,
          "operation without a free module stopped waiting");
    upper.reportArrival(first, 1);
    upper.reportArrival(first, 2);
    upper.finishOperation(first, {5});
    check(upper.placeReadyOperations().size() == 2 && upper.hasPlacement(second),
          "waiting operation was not placed after the module was released");
}

void sharedDropletIsReservedOnce() {
    using dmf::InputRef;
    using dmf::OperationType;
    dmf::BlockSpec block{
        "start", 1,
        {{"first", OperationType::Detect, {InputRef::entry(0)}, 1},
         {"second", OperationType::Detect, {InputRef::entry(0)}, 1}},
        std::nullopt};
    dmf::DynamicDag dag({"start", {{"start", std::move(block)}}});
    const auto start = dag.activateStart({1});
    const auto first = dag.operationFor(start, "first");
    const auto second = dag.operationFor(start, "second");
    dmf::UpperLayer upper(dag, dmf::VirtualChip(2, 1),
                          {{OperationType::Detect, {3, 3}}}, {{1, {0, 4}}});

    check(upper.placeReadyOperations().size() == 1 && upper.hasPlacement(first) &&
              !upper.hasPlacement(second) && dag.inputsIfReady(second) == std::nullopt,
          "one droplet was assigned to two operations");
}

void failedFeedbackKeepsPlacement() {
    dmf::DynamicDag dag(mixThenDetect());
    const auto start = dag.activateStart({1, 2});
    const auto mix = dag.operationFor(start, "mix");
    dmf::UpperLayer upper(dag, dmf::VirtualChip(1, 1),
                          {{dmf::OperationType::Mix, {3, 3}}},
                          {{1, {0, 4}}, {2, {2, 4}}});
    upper.placeReadyOperations();
    rejects<std::invalid_argument>([&] { upper.reportArrival(mix, 99); },
                                   "unrelated droplet arrival was accepted");
    upper.reportArrival(mix, 1);
    upper.reportArrival(mix, 2);
    rejects<std::invalid_argument>([&] { upper.finishOperation(mix, {1}); },
                                   "reused output droplet id was accepted");
    check(upper.hasPlacement(mix) && upper.activeBlockages().size() == 1 &&
              dag.operation(mix).state == dmf::OperationState::Running,
          "invalid completion feedback released an active module");
}

void nearestTieUsesStableOrder() {
    dmf::VirtualChip chip(2, 1);
    const auto chosen = chip.nearestFree({{7, 4}}, {3, 3});
    check(chosen && chosen->id == 1, "equal-distance tie was not deterministic");
    chip.reserve(1, 10);
    check(chip.nearestFree({{7, 4}}, {3, 3})->id == 2,
          "reserved module was selected again");
}

}  // namespace

int main() {
    try {
        operationWaitsForBothArrivals();
        occupiedModuleDefersAnotherOperation();
        sharedDropletIsReservedOnce();
        failedFeedbackKeepsPlacement();
        nearestTieUsesStableOrder();
        std::cout << "placement tests: passed\n";
    } catch (const std::exception& error) {
        std::cerr << "placement tests: " << error.what() << '\n';
        return 1;
    }
}
