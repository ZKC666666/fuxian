#include "dmf/execution_loop.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

dmf::DecisionSpec decisionFor(const std::string& operation) {
    return {operation, {{"output", {"output", {0}}},
                        {"waste", {"waste", {0}}}}};
}

dmf::BlockSpec terminalBlock(const std::string& name) {
    const auto type = name == "output" ? dmf::OperationType::Output
                                       : dmf::OperationType::Waste;
    return {name, 1,
            {{"finish", type, {dmf::InputRef::entry(0)}, 0}}, {}};
}

dmf::Protocol twoDecisionProtocol() {
    dmf::BlockSpec start{
        "start", 2,
        {{"detect_a", dmf::OperationType::Detect,
          {dmf::InputRef::entry(0)}, 1},
         {"detect_b", dmf::OperationType::Detect,
          {dmf::InputRef::entry(1)}, 1}},
        {decisionFor("detect_a"), decisionFor("detect_b")}};
    return {"start", {{"start", std::move(start)},
                      {"output", terminalBlock("output")},
                      {"waste", terminalBlock("waste")}}};
}

void invalidDecisionSpecsAreRejected() {
    auto duplicate = twoDecisionProtocol();
    duplicate.blocks.at("start").decisions.push_back(decisionFor("detect_a"));
    bool rejected = false;
    try { dmf::DynamicDag dag(std::move(duplicate)); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "one Detect accepted two decisions");

    auto missing = twoDecisionProtocol();
    missing.blocks.at("start").decisions.at(1).operation = "unknown";
    rejected = false;
    try { dmf::DynamicDag dag(std::move(missing)); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "decision accepted an unknown Detect");

    auto bad_target = twoDecisionProtocol();
    bad_target.blocks.at("start").decisions.at(1)
        .outcomes.at("waste").target_block = "unknown";
    rejected = false;
    try { dmf::DynamicDag dag(std::move(bad_target)); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "second decision accepted an unknown CFG target");
}

void independentDecisionsActivateSeparately() {
    dmf::DynamicDag dag(twoDecisionProtocol());
    const auto start = dag.activateStart({1, 2});
    const auto detect_a = dag.operationFor(start, "detect_a");
    const auto detect_b = dag.operationFor(start, "detect_b");
    dag.reserveOperation(detect_a);
    dag.reserveOperation(detect_b);
    dag.startOperation(detect_a);
    dag.startOperation(detect_b);

    dag.finishOperation(detect_a, {3}, "output");
    check(dag.blockCount() == 1 &&
              dag.operation(detect_b).state == dmf::OperationState::Running,
          "first decision affected the other running Detect");
    const auto first_branches = dag.advanceCycle();
    check(first_branches.size() == 1 &&
              dag.blockName(first_branches.front()) == "output" &&
              dag.reserveOperation(dag.operationFor(first_branches.front(), "finish")) ==
                  std::vector<int>{3},
          "first branch waited for the other Detect or received wrong input");

    dag.finishOperation(detect_b, {4}, "waste");
    const auto second_branches = dag.advanceCycle();
    check(second_branches.size() == 1 &&
              dag.blockName(second_branches.front()) == "waste" &&
              dag.reserveOperation(dag.operationFor(second_branches.front(), "finish")) ==
                  std::vector<int>{4},
          "second branch did not use its own decision and droplet");
    for (const auto branch : {first_branches.front(), second_branches.front()}) {
        const auto operation = dag.operationFor(branch, "finish");
        dag.startOperation(operation);
        dag.finishOperation(operation, {});
    }
    check(dag.allDone() && dag.blockCount() == 3,
          "two independent branches did not complete");
}

void simultaneousFeedbackActivatesTwoBranches() {
    dmf::DynamicDag dag(twoDecisionProtocol());
    const auto start = dag.activateStart({1, 2});
    const auto a = dag.operationFor(start, "detect_a");
    const auto b = dag.operationFor(start, "detect_b");
    dag.reserveOperation(a);
    dag.reserveOperation(b);
    dag.startOperation(a);
    dag.startOperation(b);
    dag.finishOperation(a, {3}, "waste");
    dag.finishOperation(b, {4}, "output");
    const auto branches = dag.advanceCycle();
    check(branches.size() == 2 && dag.blockName(branches.at(0)) == "waste" &&
              dag.blockName(branches.at(1)) == "output" &&
              dag.reserveOperation(dag.operationFor(branches.at(0), "finish")) ==
                  std::vector<int>{3} &&
              dag.reserveOperation(dag.operationFor(branches.at(1), "finish")) ==
                  std::vector<int>{4},
          "same-cycle feedback lost or crossed a decision branch");
}

void batchBarrierDelaysBothDecisions() {
    dmf::DynamicDag dag(twoDecisionProtocol());
    const auto start = dag.activateStart({1, 2});
    const auto a = dag.operationFor(start, "detect_a");
    const auto b = dag.operationFor(start, "detect_b");
    dag.reserveOperation(a);
    dag.reserveOperation(b);
    dag.startOperation(a);
    dag.startOperation(b);
    dag.finishOperation(a, {3}, "output");

    check(!dag.activatedOperationsComplete() && dag.advanceCycle(false).empty() &&
              dag.blockCount() == 1,
          "batch branch activated before the second decision completed");
    dag.finishOperation(b, {4}, "waste");
    check(dag.activatedOperationsComplete(), "completed batch still reports active work");
    const auto branches = dag.advanceCycle(true);
    check(branches.size() == 2 && dag.blockName(branches[0]) == "output" &&
              dag.blockName(branches[1]) == "waste" &&
              dag.reserveOperation(dag.operationFor(branches[0], "finish")) ==
                  std::vector<int>{3} &&
              dag.reserveOperation(dag.operationFor(branches[1], "finish")) ==
                  std::vector<int>{4},
          "batch release lost a branch or its droplet");
}

void delayedBranchDoesNotDelayOrdinaryOutput() {
    using dmf::InputRef;
    using dmf::OperationType;
    dmf::BlockSpec start{
        "start", 3,
        {{"produce", OperationType::Mix,
          {InputRef::entry(0), InputRef::entry(1)}, 1},
         {"consume", OperationType::Detect,
          {InputRef::output("produce")}, 1},
         {"branch", OperationType::Detect, {InputRef::entry(2)}, 1}},
        {decisionFor("branch")}};
    dmf::DynamicDag dag({"start", {{"start", std::move(start)},
                                    {"output", terminalBlock("output")},
                                    {"waste", terminalBlock("waste")}}});
    const auto start_id = dag.activateStart({1, 2, 3});
    const auto produce = dag.operationFor(start_id, "produce");
    const auto consume = dag.operationFor(start_id, "consume");
    const auto branch = dag.operationFor(start_id, "branch");
    dag.reserveOperation(produce);
    dag.reserveOperation(branch);
    dag.startOperation(produce);
    dag.startOperation(branch);
    dag.finishOperation(produce, {4});
    dag.finishOperation(branch, {5}, "output");

    check(dag.advanceCycle(false).empty() &&
              dag.inputsIfReady(consume) == std::vector<int>{4},
          "batch barrier delayed an ordinary operation input");
    dag.reserveOperation(consume);
    dag.startOperation(consume);
    dag.finishOperation(consume, {6});
    const auto activated = dag.advanceCycle(true);
    check(activated.size() == 1 &&
              dag.reserveOperation(dag.operationFor(activated.front(), "finish")) ==
                  std::vector<int>{5},
          "delayed branch lost its own output droplet");
}

dmf::Protocol completeParallelProtocol() {
    using dmf::InputRef;
    using dmf::OperationType;
    dmf::BlockSpec start{
        "start", 0,
        {{"source_a1", OperationType::Dispense, {}, 1},
         {"source_a2", OperationType::Dispense, {}, 1},
         {"source_b1", OperationType::Dispense, {}, 1},
         {"source_b2", OperationType::Dispense, {}, 1},
         {"mix_a", OperationType::Mix,
          {InputRef::output("source_a1"), InputRef::output("source_a2")}, 1},
         {"mix_b", OperationType::Mix,
          {InputRef::output("source_b1"), InputRef::output("source_b2")}, 1},
         {"detect_a", OperationType::Detect, {InputRef::output("mix_a")}, 1},
         {"detect_b", OperationType::Detect, {InputRef::output("mix_b")}, 1}},
        {decisionFor("detect_a"), decisionFor("detect_b")}};
    return {"start", {{"start", std::move(start)},
                      {"output", terminalBlock("output")},
                      {"waste", terminalBlock("waste")}}};
}

dmf::CompletionFeedback feedback(const dmf::OperationInstance& operation,
                                 const std::string& a, const std::string& b) {
    const std::array<std::string, 8> names{
        "source_a1", "source_a2", "source_b1", "source_b2",
        "mix_a", "mix_b", "detect_a", "detect_b"};
    const auto found = std::find(names.begin(), names.end(), operation.spec_id);
    if (found == names.end()) return {{}, std::nullopt};
    const auto id = static_cast<int>(std::distance(names.begin(), found)) + 1;
    if (operation.spec_id == "detect_a") return {{id}, a};
    if (operation.spec_id == "detect_b") return {{id}, b};
    return {{id}, std::nullopt};
}

void completePhysicalScenario(const std::string& a, const std::string& b,
                              dmf::BranchActivationMode mode) {
    dmf::DynamicDag dag(completeParallelProtocol());
    const auto start = dag.activateStart({});
    const auto detect_b = dag.operationFor(start, "detect_b");
    dmf::DropletPositions positions;
    dmf::VirtualChip chip(4, 2);
    dmf::UpperLayer upper(
        dag, chip,
        {{dmf::OperationType::Dispense, {1, 1, 1}},
         {dmf::OperationType::Mix, {3, 3, 3}},
         {dmf::OperationType::Detect, {3, 3, 2}}},
        positions,
        {{dmf::OperationType::Output, {26, 0}},
         {dmf::OperationType::Waste, {0, 14}}},
        {{"source_a1", {0, 1}}, {"source_a2", {0, 7}},
         {"source_b1", {26, 7}}, {"source_b2", {26, 13}}});
    dmf::Router router(chip.width(), chip.height(), positions);
    dmf::ExecutionLoop loop(
        dag, upper, router,
        [&](int operation) { return feedback(dag.operation(operation), a, b); },
        {}, {}, mode);

    bool a_activated_before_b_completed = false;
    int first_branch_step = 0;
    int b_complete_step = 0;
    std::vector<std::string> activated;
    int batch_plans = 0;
    std::size_t planned_steps = 0;
    std::size_t replayed_steps = 0;
    std::vector<int> terminal_starts;
    int steps = 0;
    for (; steps < 300 && !loop.allDone(); ++steps) {
        const auto report = loop.step();
        if (std::find(report.completed.begin(), report.completed.end(), detect_b) !=
            report.completed.end()) b_complete_step = report.step;
        if (report.batch_plans != 0) {
            check(report.activated_blocks.size() == 2 && report.step > b_complete_step,
                  "static planning began before both detection results were available");
        }
        batch_plans += report.batch_plans;
        planned_steps += report.planned_route_steps;
        replayed_steps += report.replayed_route_steps;
        if (report.replayed_route_steps != 0) {
            check(batch_plans == 1 && report.routing_microseconds == 0,
                  "terminal moves were computed again during replay");
        }
        for (const auto op : report.started) {
            const auto type = dag.operationType(op);
            if (type == dmf::OperationType::Output || type == dmf::OperationType::Waste) {
                terminal_starts.push_back(report.step);
            }
        }
        for (const auto block : report.activated_blocks) {
            if (first_branch_step == 0) first_branch_step = report.step;
            activated.push_back(dag.blockName(block));
            if (activated.size() == 1 &&
                dag.operation(detect_b).state != dmf::OperationState::Completed) {
                a_activated_before_b_completed = true;
            }
        }
        for (auto it = positions.begin(); it != positions.end(); ++it) {
            for (auto other = std::next(it); other != positions.end(); ++other) {
                const auto dx = std::abs(it->second.x - other->second.x);
                const auto dy = std::abs(it->second.y - other->second.y);
                check(std::max(dx, dy) >= 2,
                      "parallel simulation violated droplet separation");
            }
        }
    }
    check(loop.allDone() && positions.empty() && dag.blockCount() == 3 &&
              activated == std::vector<std::string>({a, b}),
          "parallel simulation did not complete both branches");
    if (mode == dmf::BranchActivationMode::Immediate) {
        check(a_activated_before_b_completed && first_branch_step < b_complete_step,
              "immediate mode waited for the second decision");
    } else {
        check(!a_activated_before_b_completed && first_branch_step > b_complete_step,
              "batch mode activated a branch before the batch completed");
    }
    if (mode == dmf::BranchActivationMode::StaticBatch) {
        check(batch_plans == 1 && planned_steps > 0 && planned_steps == replayed_steps,
              "static mode did not prepare and replay exactly one complete plan");
        check(terminal_starts.size() == 2 &&
                  (a != b || terminal_starts[1] > terminal_starts[0] + 1),
              "two droplets used a shared terminal before it was released");
    }
}

}  // namespace

int main() {
    try {
        invalidDecisionSpecsAreRejected();
        independentDecisionsActivateSeparately();
        simultaneousFeedbackActivatesTwoBranches();
        batchBarrierDelaysBothDecisions();
        delayedBranchDoesNotDelayOrdinaryOutput();
        for (const std::string& a : {"output", "waste"}) {
            for (const std::string& b : {"output", "waste"}) {
                completePhysicalScenario(a, b, dmf::BranchActivationMode::Immediate);
                completePhysicalScenario(a, b, dmf::BranchActivationMode::AfterBatch);
                completePhysicalScenario(a, b, dmf::BranchActivationMode::StaticBatch);
            }
        }
        std::cout << "parallel branch tests: passed\n";
    } catch (const std::exception& error) {
        std::cerr << "parallel branch tests: " << error.what() << '\n';
        return 1;
    }
}
