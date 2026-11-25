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
#include <string_view>
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
constexpr dmf::DropletId watched_droplet = 1;
constexpr char regression_pattern[] = "WOOOOOWWW";

std::string csvText(const std::string& value) {
    std::string quoted = "\"";
    for (const char c : value) {
        if (c == '"') quoted += '"';
        quoted += c;
    }
    return quoted + '"';
}

const char* stateName(dmf::OperationState state) {
    switch (state) {
        case dmf::OperationState::Waiting: return "waiting";
        case dmf::OperationState::Placed: return "placed";
        case dmf::OperationState::Running: return "running";
        case dmf::OperationState::Completed: return "completed";
    }
    throw std::logic_error("unknown operation state");
}

void writeTraceStep(std::ostream& out, const dmf::StepReport& report,
                    const dmf::RouteDecisionTrace& decision,
                    const dmf::DropletPositions& positions,
                    const dmf::UpperLayer& upper, dmf::OperationState mix,
                    dmf::OperationState detect) {
    const auto point = [&out](dmf::Point p) { out << '(' << p.x << ',' << p.y << ')'; };
    const auto module = [&](const dmf::VirtualModule& m) {
        out << ' ' << m.id << '@';
        point(m.top_left);
        out << ':' << m.side;
    };
    out << "step=" << report.step << " mix_0=" << stateName(mix)
        << " detect_0=" << stateName(detect)
        << " stale_uses=" << report.stale_field_decisions;
    out << " positions_end:";
    for (const auto& [droplet, position] : positions) {
        out << " d" << droplet << '@';
        point(position);
    }
    out << " | active_end:";
    for (const auto& m : upper.activeBlockages()) module(m);
    out << " | reserved_end:";
    for (const auto& m : upper.upcomingBlockages()) module(m.module);
    out << '\n';
    if (decision.action.empty()) return;

    out << "  d" << decision.droplet << ' ' << decision.action << " from=";
    point(decision.from);
    out << " target=";
    point(decision.target);
    out << " chosen=";
    point(decision.chosen);
    out << " layout=" << decision.layout_version;
    if (decision.field_version) out << " field=" << *decision.field_version;
    for (const auto& candidate : decision.candidates) {
        out << " | ";
        point(candidate.point);
        if (!candidate.rejection.empty()) {
            out << '=' << candidate.rejection;
        } else {
            out << "=score:" << candidate.cost << "/next:" << candidate.future_cost;
            if (candidate.backtracks) out << "/backtrack";
        }
    }
    out << '\n';
}

dmf::ExperimentSummary runTrial(const std::vector<std::string>& outcomes,
                                dmf::BranchActivationMode mode,
                                dmf::CostFieldMode field_mode = dmf::CostFieldMode::Asynchronous,
                                std::ostream* trace = nullptr) {
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
    dmf::Router router(chip.width(), chip.height(), positions, upper.minimumSeparation(),
                       field_mode);
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
    const auto mix = dag.operationFor(start_block, mixName(0));
    const auto detect = dag.operationFor(start_block, detectName(0));
    dmf::RouteDecisionTrace decision{watched_droplet};

    using Clock = std::chrono::steady_clock;
    for (int step = 0; step < max_steps && !loop.allDone(); ++step) {
        const auto start = Clock::now();
        const auto report = loop.step(trace ? &decision : nullptr);
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
        if (trace) {
            writeTraceStep(*trace, report, decision, positions, upper,
                           dag.operation(mix).state, dag.operation(detect).state);
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

int diagnose(const std::string& pattern) {
    if (pattern.size() != group_count || pattern.find_first_not_of("OW") != std::string::npos) {
        std::cerr << "diagnostic pattern must contain exactly nine O/W letters\n";
        return 2;
    }
    std::vector<std::string> outcomes;
    for (const char branch : pattern) outcomes.emplace_back(branch == 'O' ? "output" : "waste");
    std::ofstream output("paper_parallel_diagnosis.txt");
    if (!output) {
        std::cerr << "cannot create paper_parallel_diagnosis.txt\n";
        return 1;
    }
    bool all_complete = true;
    for (const auto field_mode : {dmf::CostFieldMode::Asynchronous,
                                  dmf::CostFieldMode::WaitForLatest}) {
        const auto label = field_mode == dmf::CostFieldMode::Asynchronous ? "async" : "fresh";
        std::ostringstream trace;
        trace << "pattern=" << pattern << " mode=immediate field_mode=" << label
              << " watched_droplet=" << watched_droplet << '\n';
        std::string result;
        try {
            const auto summary = runTrial(outcomes, dmf::BranchActivationMode::Immediate,
                                          field_mode, &trace);
            result = "complete, steps=" + std::to_string(summary.steps);
        } catch (const dmf::ExperimentTimeout& error) {
            all_complete = false;
            result = std::string("timeout: ") + error.what();
        } catch (const std::exception& error) {
            all_complete = false;
            result = std::string("failed: ") + error.what();
        }
        output << trace.str() << "result=" << result << "\n\n";
        std::cout << label << ": " << result << '\n';
    }
    if (!output) {
        std::cerr << "cannot write paper_parallel_diagnosis.txt\n";
        return 1;
    }
    std::cout << "trace: paper_parallel_diagnosis.txt\n";
    return all_complete ? 0 : 1;
}

int checkSeed1601() {
    std::vector<std::string> outcomes;
    for (const char branch : std::string_view(regression_pattern)) {
        outcomes.emplace_back(branch == 'O' ? "output" : "waste");
    }
    try {
        const auto summary = runTrial(outcomes, dmf::BranchActivationMode::Immediate,
                                      dmf::CostFieldMode::WaitForLatest);
        if (summary.route_requests != 36 || summary.route_arrivals != 36) {
            throw std::logic_error("seed 1601 should complete all 36 routing tasks");
        }
        std::cout << "seed 1601 complete, steps=" << summary.steps << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "seed 1601: " << error.what() << '\n';
        return 1;
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--check-1601") return checkSeed1601();
    if (argc > 1 && std::string(argv[1]) == "--diagnose") {
        if (argc <= 3) return diagnose(argc == 3 ? argv[2] : regression_pattern);
        std::cerr << "usage: dmf_paper_parallel_experiments --diagnose [OOOOOOOOW]\n";
        return 2;
    }
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
