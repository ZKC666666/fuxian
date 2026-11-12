#include "parallel_protocol_demo.hpp"

#include "dmf/execution_loop.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <map>
#include <stdexcept>
#include <utility>

namespace dmf::demo {

namespace {

Protocol parallelProtocol() {
    BlockSpec start{
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
        {{"detect_a", {{"output", {"output", {0}}},
                        {"waste", {"waste", {0}}}}},
         {"detect_b", {{"output", {"output", {0}}},
                        {"waste", {"waste", {0}}}}}}};
    BlockSpec output{
        "output", 1,
        {{"send", OperationType::Output, {InputRef::entry(0)}, 0}}, {}};
    BlockSpec waste{
        "waste", 1,
        {{"discard", OperationType::Waste, {InputRef::entry(0)}, 0}}, {}};
    return {"start", {{"start", std::move(start)},
                      {"output", std::move(output)},
                      {"waste", std::move(waste)}}};
}

CompletionFeedback feedbackFor(const OperationInstance& instance,
                               const std::string& first_outcome,
                               const std::string& second_outcome) {
    static const std::map<std::string, DropletId> outputs{
        {"source_a1", 1}, {"source_a2", 2},
        {"source_b1", 3}, {"source_b2", 4},
        {"mix_a", 5}, {"mix_b", 6},
        {"detect_a", 7}, {"detect_b", 8}};
    const auto found = outputs.find(instance.spec_id);
    if (found == outputs.end()) return {{}, std::nullopt};
    if (instance.spec_id == "detect_a") return {{found->second}, first_outcome};
    if (instance.spec_id == "detect_b") return {{found->second}, second_outcome};
    return {{found->second}, std::nullopt};
}

void printStep(const StepReport& report, const DynamicDag& dag,
               const std::string& first_outcome,
               const std::string& second_outcome) {
    std::cout << "step " << report.step << ": ";
    for (const auto block : report.activated_blocks) {
        std::cout << "activate " << dag.blockName(block) << " (#" << block << "); ";
    }
    for (const auto& move : report.moves) {
        if (move.from == move.to) continue;
        std::cout << "d" << move.droplet << " -> ("
                  << move.to.x << ',' << move.to.y << "); ";
    }
    for (const auto operation : report.started) {
        std::cout << "start " << dag.operation(operation).spec_id << "; ";
    }
    for (const auto operation : report.completed) {
        const auto& name = dag.operation(operation).spec_id;
        std::cout << "finish " << name;
        if (name == "detect_a") std::cout << " -> " << first_outcome;
        if (name == "detect_b") std::cout << " -> " << second_outcome;
        std::cout << "; ";
    }
    if (!report.waiting_for_output_space.empty()) {
        std::cout << "waiting for safe output space; ";
    }
    std::cout << "active blockages=" << report.active_blockages << '\n';
}

}  // namespace

void runParallelProtocol(const std::string& first_outcome,
                         const std::string& second_outcome, bool trace) {
    if ((first_outcome != "output" && first_outcome != "waste") ||
        (second_outcome != "output" && second_outcome != "waste")) {
        throw std::invalid_argument("unknown parallel protocol outcome");
    }
    DynamicDag dag(parallelProtocol());
    dag.activateStart({});
    DropletPositions positions;
    VirtualChip chip(4, 2);
    UpperLayer upper(
        dag, chip,
        {{OperationType::Dispense, {1, 1, 1}},
         {OperationType::Mix, {3, 3, 3}},
         {OperationType::Detect, {3, 3, 2}}},
        positions,
        {{OperationType::Output, {26, 0}}, {OperationType::Waste, {0, 14}}},
        {{"source_a1", {0, 1}}, {"source_a2", {0, 7}},
         {"source_b1", {26, 7}}, {"source_b2", {26, 13}}});
    Router router(chip.width(), chip.height(), positions);
    ExecutionLoop loop(dag, upper, router,
                       [&](OperationInstanceId operation) {
                           return feedbackFor(dag.operation(operation),
                                              first_outcome, second_outcome);
                       });

    using Clock = std::chrono::steady_clock;
    long long total_microseconds = 0;
    long long peak_microseconds = 0;
    int steps = 0;
    int output_wait_steps = 0;
    if (trace) {
        std::cout << "chip=" << chip.width() << 'x' << chip.height()
                  << "; group A=" << first_outcome
                  << "; group B=" << second_outcome << '\n';
    }
    for (int attempt = 0; attempt < 250 && !loop.allDone(); ++attempt) {
        const auto start = Clock::now();
        const auto report = loop.step();
        const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now() - start).count();
        total_microseconds += duration;
        peak_microseconds = std::max(peak_microseconds, duration);
        steps = report.step;
        if (!report.waiting_for_output_space.empty()) ++output_wait_steps;
        if (trace) printStep(report, dag, first_outcome, second_outcome);
    }
    if (!loop.allDone() || !positions.empty() || dag.blockCount() != 3) {
        throw std::runtime_error("parallel protocol did not finish cleanly");
    }
    std::cout << "A=" << first_outcome << ", B=" << second_outcome
              << ": completed=true, steps=" << steps
              << ", droplets remaining=" << positions.size()
              << ", blocks=" << dag.blockCount()
              << ", output wait steps=" << output_wait_steps
              << ", loop time avg/max (microseconds, excluding output)="
              << total_microseconds / steps << '/' << peak_microseconds << '\n';
}

}  // namespace dmf::demo
