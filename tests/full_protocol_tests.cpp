#include "dmf/execution_loop.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using dmf::InputRef;
using dmf::OperationType;

void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

dmf::Protocol fullProtocol() {
    dmf::BlockSpec start{
        "start", 0,
        {{"source_a", OperationType::Dispense, {}, 1},
         {"source_b", OperationType::Dispense, {}, 1},
         {"mix", OperationType::Mix,
          {InputRef::output("source_a"), InputRef::output("source_b")}, 1},
         {"split", OperationType::Split, {InputRef::output("mix")}, 2},
         {"detect", OperationType::Detect, {InputRef::output("split", 0)}, 1},
         {"discard_second", OperationType::Waste,
          {InputRef::output("split", 1)}, 0}},
        {dmf::DecisionSpec{"detect", {{"output", {"output", {0}}},
                                      {"waste", {"waste", {0}}}}}}};
    dmf::BlockSpec output{
        "output", 1,
        {{"send", OperationType::Output, {InputRef::entry(0)}, 0}},
        {}};
    dmf::BlockSpec waste{
        "waste", 1,
        {{"discard", OperationType::Waste, {InputRef::entry(0)}, 0}},
        {}};
    return {"start", {{"start", std::move(start)},
                      {"output", std::move(output)},
                      {"waste", std::move(waste)}}};
}

std::map<OperationType, dmf::ModuleProfile> profiles() {
    return {{OperationType::Dispense, {1, 1, 1}},
            {OperationType::Mix, {3, 3, 3}},
            {OperationType::Split, {3, 1, 2}},
            {OperationType::Detect, {3, 3, 2}}};
}

void invalidSourceAndSplitShapesAreRejected() {
    auto invalid = fullProtocol();
    invalid.blocks.at("start").operations.at(0).output_count = 2;
    bool rejected = false;
    try { dmf::DynamicDag dag(std::move(invalid)); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "Dispense with two outputs was accepted");

    invalid = fullProtocol();
    invalid.blocks.at("start").operations.at(3).output_count = 1;
    rejected = false;
    try { dmf::DynamicDag dag(std::move(invalid)); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "Split with one output was accepted");
}

void occupiedDispensePortWaitsAndRetries() {
    dmf::BlockSpec block{"start", 0,
                         {{"source", OperationType::Dispense, {}, 1}},
                         {}};
    dmf::DynamicDag dag({"start", {{"start", std::move(block)}}});
    const auto instance = dag.activateStart({});
    const auto source = dag.operationFor(instance, "source");
    dmf::DropletPositions positions{{99, {0, 1}}};
    dmf::UpperLayer upper(dag, dmf::VirtualChip(1, 1),
                          {{OperationType::Dispense, {1, 1, 1}}}, positions,
                          {}, {{"source", {0, 0}}});
    check(upper.placeReadyOperations().empty() && !upper.hasPlacement(source),
          "occupied port placed Dispense");
    positions.at(99) = {3, 0};
    upper.placeReadyOperations();
    const auto started = upper.tryStartPlacedOperations();
    check(started.size() == 1 && started.front() == source,
          "Dispense did not start after its port cleared");
    bool rejected = false;
    try { upper.finishOperation(source, {99}); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected && positions.at(99) == dmf::Point{3, 0},
          "Dispense overwrote an unrelated droplet id");
    upper.finishOperation(source, {1});
    check(positions.at(1) == dmf::Point{0, 0} &&
              dag.operation(source).state == dmf::OperationState::Completed,
          "Dispense did not create its droplet at the configured port");
}

void sharedDispensePortIsSerialized() {
    dmf::BlockSpec block{
        "start", 0,
        {{"first", OperationType::Dispense, {}, 1},
         {"second", OperationType::Dispense, {}, 1}},
        {}};
    dmf::DynamicDag dag({"start", {{"start", std::move(block)}}});
    const auto instance = dag.activateStart({});
    const auto first = dag.operationFor(instance, "first");
    const auto second = dag.operationFor(instance, "second");
    dmf::DropletPositions positions;
    dmf::UpperLayer upper(dag, dmf::VirtualChip(1, 1),
                          {{OperationType::Dispense, {1, 1, 1}}}, positions,
                          {}, {{"first", {0, 0}}, {"second", {0, 0}}});
    upper.placeReadyOperations();
    check(upper.hasPlacement(first) && !upper.hasPlacement(second),
          "two Dispense operations reserved the same port");
    upper.tryStartPlacedOperations();
    upper.finishOperation(first, {1});
    upper.placeReadyOperations();
    check(!upper.hasPlacement(second),
          "port was reused while the first droplet remained there");
    positions.at(1) = {3, 0};
    upper.placeReadyOperations();
    check(upper.hasPlacement(second),
          "port did not become available after the first droplet moved");
    upper.tryStartPlacedOperations();
    upper.finishOperation(second, {2});
    check(positions.at(2) == dmf::Point{0, 0},
          "second Dispense did not use the shared port");
}

void splitWaitsForTwoSafeOutputCells() {
    dmf::BlockSpec block{"start", 1,
                         {{"split", OperationType::Split,
                           {InputRef::entry(0)}, 2}},
                         {}};
    dmf::DynamicDag dag({"start", {{"start", std::move(block)}}});
    const auto instance = dag.activateStart({1});
    const auto split = dag.operationFor(instance, "split");
    dmf::DropletPositions positions{{1, {4, 4}}, {99, {2, 4}}};
    dmf::VirtualChip chip(1, 1);
    dmf::UpperLayer upper(dag, chip,
                          {{OperationType::Split, {3, 1, 1}}}, positions);
    dmf::Router router(chip.width(), chip.height(), positions);
    int feedback_calls = 0;
    dmf::ExecutionLoop loop(
        dag, upper, router,
        [&](int) {
            ++feedback_calls;
            return dmf::CompletionFeedback{{2, 3}, std::nullopt};
        });

    const auto first = loop.step();
    check(first.started.size() == 1 && first.started.front() == split,
          "Split did not start after its input arrived");
    for (int step = 0; step < 2; ++step) {
        const auto waiting = loop.step();
        check(waiting.completed.empty() &&
                  waiting.waiting_for_output_space.size() == 1 &&
                  waiting.waiting_for_output_space.front() == split,
              "Split released outputs into an occupied safety zone");
    }
    check(feedback_calls == 1,
          "waiting for output space requested new droplet ids repeatedly");
    bool rejected = false;
    try { loop.completeEarly(split, {{7, 8}, std::nullopt}); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected && feedback_calls == 1,
          "pending Split feedback changed while outputs were waiting");
    positions.at(99) = {0, 0};
    const auto released = loop.step();
    check(released.completed.size() == 1 &&
              positions.at(2) == dmf::Point{3, 4} &&
              positions.at(3) == dmf::Point{5, 4} &&
              positions.count(1) == 0 && feedback_calls == 1,
          "Split did not release two separated droplets after space cleared");
}

void completeBranch(const std::string& outcome) {
    dmf::DynamicDag dag(fullProtocol());
    const auto start = dag.activateStart({});
    const auto split = dag.operationFor(start, "split");
    const auto detect = dag.operationFor(start, "detect");
    dmf::DropletPositions positions;
    dmf::VirtualChip chip(2, 1);
    dmf::UpperLayer upper(
        dag, chip, profiles(), positions,
        {{OperationType::Output, {14, 4}}, {OperationType::Waste, {0, 8}}},
        {{"source_a", {0, 1}}, {"source_b", {0, 7}}});
    dmf::Router router(chip.width(), chip.height(), positions);
    dmf::ExecutionLoop loop(
        dag, upper, router,
        [&dag, &outcome](int operation) {
            const auto& name = dag.operation(operation).spec_id;
            if (name == "source_a") return dmf::CompletionFeedback{{1}, std::nullopt};
            if (name == "source_b") return dmf::CompletionFeedback{{2}, std::nullopt};
            if (name == "mix") return dmf::CompletionFeedback{{3}, std::nullopt};
            if (name == "split") return dmf::CompletionFeedback{{4, 5}, std::nullopt};
            if (name == "detect") return dmf::CompletionFeedback{{6}, outcome};
            return dmf::CompletionFeedback{{}, std::nullopt};
        });

    bool saw_split = false;
    bool saw_branch = false;
    for (int step = 0; step < 180 && !loop.allDone(); ++step) {
        const auto report = loop.step();
        if (std::find(report.completed.begin(), report.completed.end(), split) !=
            report.completed.end()) {
            saw_split = true;
            check(positions.at(4) == dmf::Point{3, 4} &&
                      positions.at(5) == dmf::Point{5, 4},
                  "Split placed its two products incorrectly");
        }
        if (std::find(report.completed.begin(), report.completed.end(), detect) !=
            report.completed.end()) {
            saw_branch = true;
        }
        for (const auto& [id, point] : positions) {
            for (const auto& [other_id, other] : positions) {
                if (id >= other_id) continue;
                check(std::max(std::abs(point.x - other.x),
                               std::abs(point.y - other.y)) >= 2,
                      "full protocol violated droplet separation");
            }
        }
    }
    check(saw_split && saw_branch && loop.allDone() && positions.empty() &&
              dag.blockCount() == 2,
          "Dispense-Mix-Split-Detect terminal protocol did not finish");
    check(dag.blockName(2) == outcome,
          "Detect activated the wrong dynamic DAG branch");
}

}  // namespace

int main() {
    try {
        invalidSourceAndSplitShapesAreRejected();
        occupiedDispensePortWaitsAndRetries();
        sharedDispensePortIsSerialized();
        splitWaitsForTwoSafeOutputCells();
        completeBranch("output");
        completeBranch("waste");
        std::cout << "full protocol tests: passed\n";
    } catch (const std::exception& error) {
        std::cerr << "full protocol tests: " << error.what() << '\n';
        return 1;
    }
}
