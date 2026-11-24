#include "dmf/experiment_metrics.hpp"
#include "paper_parallel_case.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using dmf::paper::group_count;
using dmf::paper::sourceName;
using dmf::paper::mixName;
using dmf::paper::detectName;
using dmf::paper::sourcePorts;
using dmf::paper::outcomesFor;
using dmf::paper::feedbackFor;
constexpr int max_steps = 1000;

std::string csvText(const std::string& value) {
    std::string quoted = "\"";
    for (const char c : value) {
        if (c == '"') quoted += '"';
        quoted += c;
    }
    return quoted + '"';
}

dmf::ExperimentSummary runTrial(const std::vector<std::string>& outcomes,
                                dmf::BranchActivationMode mode) {
    dmf::DynamicDag dag(dmf::paper::makeProtocol());
    const auto start_block = dag.activateStart({});
    if (dag.operationCount() != 4 * group_count) {
        throw std::logic_error("wrong number of generated operations");
    }
    dmf::DropletPositions positions;
    dmf::VirtualChip chip(4, 4, 3, 30, 30);
    dmf::UpperLayer upper(
        dag, chip,
        {{dmf::OperationType::Dispense, {1, 1, 1}},
         {dmf::OperationType::Mix, {3, 3, 3}},
         {dmf::OperationType::Detect, {3, 3, 2}}},
        positions,
        {{dmf::OperationType::Output, {29, 0}},
         {dmf::OperationType::Waste, {0, 29}}},
        sourcePorts());
    dmf::Router router(chip.width(), chip.height(), positions);
    const auto feedback = feedbackFor(outcomes);
    dmf::ExecutionLoop loop(
        dag, upper, router,
        [&](int operation) {
            const auto& name = dag.operation(operation).spec_id;
            const auto found = feedback.find(name);
            return found == feedback.end() ? dmf::CompletionFeedback{{}, std::nullopt}
                                           : found->second;
        }, {}, {}, mode);
    dmf::ExperimentMetrics metrics(chip.width(), chip.height(),
                                   router.minimumSeparation());
    std::map<dmf::DropletId, dmf::Point> active_targets;
    int activated_outputs = 0;
    int activated_waste = 0;

    using Clock = std::chrono::steady_clock;
    for (int step = 0; step < max_steps && !loop.allDone(); ++step) {
        const auto start = Clock::now();
        const auto report = loop.step();
        const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now() - start).count();
        metrics.observe(report, positions, micros);
        for (const auto block : report.activated_blocks) {
            if (dag.blockName(block) == "output") ++activated_outputs;
            else if (dag.blockName(block) == "waste") ++activated_waste;
        }
        for (const auto& task : report.routing_tasks) {
            active_targets[task.droplet] = task.target;
        }
        for (const auto& arrival : report.route_arrivals) {
            active_targets.erase(arrival.droplet);
        }
    }
    if (!loop.allDone()) {
        int waiting = 0;
        int placed = 0;
        int running = 0;
        int completed = 0;
        for (int group = 0; group < group_count; ++group) {
            for (const auto& name : {sourceName(group, 'a'), sourceName(group, 'b'),
                                     mixName(group), detectName(group)}) {
                switch (dag.operation(dag.operationFor(start_block, name)).state) {
                    case dmf::OperationState::Waiting: ++waiting; break;
                    case dmf::OperationState::Placed: ++placed; break;
                    case dmf::OperationState::Running: ++running; break;
                    case dmf::OperationState::Completed: ++completed; break;
                }
            }
        }
        std::ostringstream message;
        message << "nine-mix trial timed out: waiting=" << waiting
                << " placed=" << placed << " running=" << running
                << " completed=" << completed << " droplets=" << positions.size()
                << " routes_active=" << router.hasTasks()
                << " blocks=" << dag.blockCount();
        for (const auto& [droplet, position] : positions) {
            message << " d" << droplet << "@" << position.x << ':' << position.y;
            const auto target = active_targets.find(droplet);
            if (target != active_targets.end()) {
                message << "->" << target->second.x << ':' << target->second.y;
            }
        }
        for (int group = 0; group < group_count; ++group) {
            for (const auto& name : {mixName(group), detectName(group)}) {
                const auto state = dag.operation(dag.operationFor(start_block, name)).state;
                if (state != dmf::OperationState::Completed) {
                    message << ' ' << name << '=' << static_cast<int>(state);
                }
            }
        }
        throw dmf::ExperimentTimeout(message.str());
    }
    if (!positions.empty() || dag.blockCount() != group_count + 1 ||
        dag.operationCount() != 5 * group_count) {
        throw std::logic_error("nine-mix trial did not finish cleanly");
    }
    const int expected_outputs = static_cast<int>(std::count(
        outcomes.begin(), outcomes.end(), "output"));
    if (activated_outputs != expected_outputs ||
        activated_waste != group_count - expected_outputs) {
        throw std::logic_error("nine-mix trial activated wrong branch counts");
    }
    const auto result = metrics.summary();
    if (result.route_requests != result.route_arrivals) {
        throw std::logic_error("nine-mix trial has unfinished routes");
    }
    return result;
}

struct RunCounts {
    int complete = 0;
    int timeout = 0;
    int failed = 0;
};

RunCounts runExperiments(std::ostream& out, bool scenario_one_only) {
    RunCounts counts;
    out << "scenario,output_probability,seed,outcome_pattern,realized_outputs,mode,status,"
           "steps,route_requests,route_arrivals,throughput,plan_step_total_us,"
           "loop_total_us,error\n";
    out << std::fixed << std::setprecision(6);
    const std::array<int, 4> probabilities{100, 90, 70, 50};
    for (int scenario = 0; scenario < (scenario_one_only ? 1 : 4); ++scenario) {
        for (int repeat = 0; repeat < 5; ++repeat) {
            const unsigned seed = 1400U + static_cast<unsigned>(100 * scenario + repeat);
            const auto outcomes = outcomesFor(probabilities[scenario], seed);
            const int realized = static_cast<int>(std::count(outcomes.begin(),
                                                             outcomes.end(), "output"));
            std::string pattern;
            for (const auto& outcome : outcomes) {
                pattern += outcome == "output" ? 'O' : 'W';
            }
            for (const auto mode : {dmf::BranchActivationMode::Immediate,
                                    dmf::BranchActivationMode::AfterBatch}) {
                out << scenario + 1 << ',' << probabilities[scenario] << ',' << seed
                    << ',' << pattern << ',' << realized << ','
                    << (mode == dmf::BranchActivationMode::Immediate ? "immediate"
                                                                     : "after_batch") << ',';
                try {
                    const auto result = runTrial(outcomes, mode);
                    ++counts.complete;
                    out << "complete," << result.steps << ',' << result.route_requests
                        << ',' << result.route_arrivals << ','
                        << result.routingThroughput() << ','
                        << result.routing_microseconds << ','
                        << result.loop_microseconds << ",\"\"\n";
                } catch (const dmf::ExperimentTimeout& error) {
                    ++counts.timeout;
                    out << "timeout,,,,,,," << csvText(error.what()) << '\n';
                } catch (const std::exception& error) {
                    ++counts.failed;
                    out << "failed,,,,,,," << csvText(error.what()) << '\n';
                }
            }
        }
    }
    return counts;
}

}  // namespace

int main(int argc, char** argv) {
    const bool all_scenarios = argc > 1 && std::string(argv[1]) == "--all";
    if (argc > (all_scenarios ? 3 : 2)) {
        std::cerr << "usage: dmf_paper_parallel_experiments [--all] [output.csv]\n";
        return 2;
    }
    const std::string path = argc == (all_scenarios ? 3 : 2)
                                 ? argv[argc - 1]
                                 : (all_scenarios ? "paper_parallel_trials.csv"
                                                  : "paper_parallel_scenario1.csv");
    std::ofstream output(path);
    if (!output) {
        std::cerr << "cannot create " << path << '\n';
        return 1;
    }
    const auto counts = runExperiments(output, !all_scenarios);
    if (!output) {
        std::cerr << "failed to write " << path << '\n';
        return 1;
    }
    std::cout << "wrote " << path << "; complete=" << counts.complete
              << ", timeout=" << counts.timeout
              << ", failed=" << counts.failed << '\n';
    return counts.timeout == 0 && counts.failed == 0 ? 0 : 1;
}
