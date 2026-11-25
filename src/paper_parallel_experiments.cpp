#include "dmf/experiment_metrics.hpp"
#include "paper_parallel_case.hpp"

#include <algorithm>
#include <array>
#include <charconv>
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
using dmf::paper::ParallelCase;
constexpr int max_steps = 1000;
constexpr dmf::DropletId watched_droplet = 1;
constexpr char regression_pattern[] = "WOOOOOWWW";

const char* modeName(dmf::BranchActivationMode mode) {
    switch (mode) {
        case dmf::BranchActivationMode::Immediate: return "immediate";
        case dmf::BranchActivationMode::AfterBatch: return "after_batch";
        case dmf::BranchActivationMode::StaticBatch: return "static_batch";
    }
    throw std::logic_error("unknown branch mode");
}

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
    if (report.batch_plans != 0) out << " planned_route_steps=" << report.planned_route_steps;
    if (report.replayed_route_steps != 0) out << " replayed_terminal_moves";
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

struct TrialResult {
    dmf::ExperimentSummary metrics;
    int detour_searches;
    int synchronous_field_refreshes;
    std::string timeout;
};

TrialResult runTrial(const ParallelCase& config, const std::vector<std::string>& outcomes,
                     dmf::BranchActivationMode mode,
                     dmf::CostFieldMode field_mode = dmf::CostFieldMode::Asynchronous,
                     std::ostream* trace = nullptr) {
    if (outcomes.size() != static_cast<std::size_t>(config.groups)) {
        throw std::invalid_argument("branch pattern does not match group count");
    }
    dmf::DynamicDag dag(dmf::paper::makeProtocol(config));
    const auto start_block = dag.activateStart({});
    if (dag.operationCount() != 4 * config.groups) {
        throw std::logic_error("wrong number of generated operations");
    }
    dmf::DropletPositions positions;
    dmf::VirtualChip chip(config.modules_per_side, config.modules_per_side, 3,
                         config.chip_side, config.chip_side);
    dmf::UpperLayer upper(
        dag, chip,
        {{dmf::OperationType::Dispense, {1, 1, 1}},
         {dmf::OperationType::Mix, {3, 3, 3}},
         {dmf::OperationType::Detect, {3, 3, 2}}},
        positions,
        {{dmf::OperationType::Output, config.outputPort()},
         {dmf::OperationType::Waste, config.wastePort()}},
        sourcePorts(config));
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
    TrialResult result{metrics.summary(), router.detourSearches(),
                       router.synchronousFieldRefreshes(), {}};
    if (!loop.allDone()) {
        int waiting = 0;
        int placed = 0;
        int running = 0;
        int completed = 0;
        for (int group = 0; group < config.groups; ++group) {
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
        message << config.groups << "-mix trial timed out: waiting=" << waiting
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
        for (int group = 0; group < config.groups; ++group) {
            for (const auto& name : {mixName(group), detectName(group)}) {
                const auto state = dag.operation(dag.operationFor(start_block, name)).state;
                if (state != dmf::OperationState::Completed) {
                    message << ' ' << name << '=' << static_cast<int>(state);
                }
            }
        }
        result.timeout = message.str();
        return result;
    }
    if (!positions.empty() || dag.blockCount() != config.groups + 1 ||
        dag.operationCount() != 5 * config.groups) {
        throw std::logic_error("parallel trial did not finish cleanly");
    }
    const int expected_outputs = static_cast<int>(std::count(
        outcomes.begin(), outcomes.end(), "output"));
    if (activated_outputs != expected_outputs ||
        activated_waste != config.groups - expected_outputs) {
        throw std::logic_error("parallel trial activated wrong branch counts");
    }
    const int expected_routes = 4 * config.groups;
    if (result.metrics.route_requests != expected_routes ||
        result.metrics.route_arrivals != expected_routes) {
        throw std::logic_error("parallel trial completed with unexpected routing counts");
    }
    if (mode == dmf::BranchActivationMode::StaticBatch &&
        (result.metrics.batch_plans != 1 ||
         result.metrics.planned_route_steps !=
             static_cast<std::size_t>(result.metrics.replayed_route_steps))) {
        throw std::logic_error("static trial did not replay its complete terminal plan");
    }
    return result;
}

struct RunCounts {
    int complete = 0;
    int timeout = 0;
    int failed = 0;
};

RunCounts runExperiments(std::ostream& out, const ParallelCase& config,
                         bool scenario_one_only, bool compare_static) {
    RunCounts counts;
    out << "chip_side,groups,scenario,output_probability,seed,outcome_pattern,realized_outputs,mode,status,"
           "steps,route_requests,route_arrivals,throughput,plan_step_total_us,"
           "loop_total_us,detour_searches,synchronous_field_refreshes,batch_planning_us,"
           "replay_us,batch_plans,planned_route_steps,replayed_route_steps,error\n";
    out << std::fixed << std::setprecision(6);
    const std::array<int, 4> probabilities{100, 90, 70, 50};
    for (int scenario = 0; scenario < (scenario_one_only ? 1 : 4); ++scenario) {
        for (int repeat = 0; repeat < 5; ++repeat) {
            const unsigned seed = 1400U + static_cast<unsigned>(100 * scenario + repeat);
            const auto outcomes = outcomesFor(config, probabilities[scenario], seed);
            const int realized = static_cast<int>(std::count(outcomes.begin(),
                                                             outcomes.end(), "output"));
            std::string pattern;
            for (const auto& outcome : outcomes) {
                pattern += outcome == "output" ? 'O' : 'W';
            }
            for (const auto mode : {dmf::BranchActivationMode::Immediate,
                                    dmf::BranchActivationMode::AfterBatch,
                                    dmf::BranchActivationMode::StaticBatch}) {
                if (!compare_static && mode == dmf::BranchActivationMode::StaticBatch) continue;
                out << config.chip_side << ',' << config.groups << ',' << scenario + 1
                    << ',' << probabilities[scenario] << ',' << seed
                    << ',' << pattern << ',' << realized << ','
                    << modeName(mode) << ',';
                try {
                    const auto trial = runTrial(config, outcomes, mode);
                    const auto& result = trial.metrics;
                    if (trial.timeout.empty()) ++counts.complete;
                    else ++counts.timeout;
                    out << (trial.timeout.empty() ? "complete," : "timeout,")
                        << result.steps << ',' << result.route_requests
                        << ',' << result.route_arrivals << ','
                        << result.routingThroughput() << ','
                        << result.routing_microseconds << ',' << result.loop_microseconds
                        << ',' << trial.detour_searches
                        << ',' << trial.synchronous_field_refreshes
                        << ',' << result.batch_planning_microseconds << ',' << result.replay_microseconds
                        << ',' << result.batch_plans << ',' << result.planned_route_steps
                        << ',' << result.replayed_route_steps
                        << ',' << csvText(trial.timeout) << '\n';
                } catch (const std::exception& error) {
                    ++counts.failed;
                    out << "failed,,,,,,,,,,,,,," << csvText(error.what()) << '\n';
                }
            }
        }
    }
    return counts;
}

int diagnose(const ParallelCase& config, const std::string& pattern,
             dmf::BranchActivationMode mode) {
    if (pattern.size() != static_cast<std::size_t>(config.groups) ||
        pattern.find_first_not_of("OW") != std::string::npos) {
        std::cerr << "diagnostic pattern must contain exactly " << config.groups << " O/W letters\n";
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
        trace << "groups=" << config.groups << " pattern=" << pattern
              << " mode=" << modeName(mode)
              << " field_mode=" << label
              << " watched_droplet=" << watched_droplet << '\n';
        std::string result;
        try {
            const auto trial = runTrial(config, outcomes, mode, field_mode, &trace);
            if (trial.timeout.empty()) {
                result = "complete, steps=" + std::to_string(trial.metrics.steps);
            } else {
                all_complete = false;
                result = "timeout: " + trial.timeout;
            }
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
        const auto trial = runTrial(*dmf::paper::caseForGroups(group_count), outcomes,
                                    dmf::BranchActivationMode::Immediate,
                                    dmf::CostFieldMode::WaitForLatest);
        if (!trial.timeout.empty()) throw dmf::ExperimentTimeout(trial.timeout);
        std::cout << "seed 1601 complete, steps=" << trial.metrics.steps << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "seed 1601: " << error.what() << '\n';
        return 1;
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--check-1601") return checkSeed1601();
    int arg = 1;
    auto config = dmf::paper::caseForGroups(group_count);
    if (arg < argc && std::string_view(argv[arg]) == "--groups") {
        if (++arg == argc) {
            std::cerr << "--groups requires a supported group count\n";
            return 2;
        }
        const std::string_view count(argv[arg++]);
        int groups = 0;
        const auto parsed = std::from_chars(count.data(), count.data() + count.size(), groups);
        if (parsed.ec != std::errc{} || parsed.ptr != count.data() + count.size() ||
            !(config = dmf::paper::caseForGroups(groups))) {
            std::cerr << "unsupported group count: " << count << '\n';
            return 2;
        }
    }
    if (arg < argc && std::string_view(argv[arg]) == "--diagnose") {
        ++arg;
        const std::string pattern = arg < argc ? argv[arg++] :
            (config->groups == group_count ? regression_pattern : std::string(config->groups, 'O'));
        auto mode = dmf::BranchActivationMode::Immediate;
        if (arg < argc && std::string_view(argv[arg]) == "after_batch") {
            mode = dmf::BranchActivationMode::AfterBatch;
            ++arg;
        } else if (arg < argc && std::string_view(argv[arg]) == "static_batch") {
            mode = dmf::BranchActivationMode::StaticBatch;
            ++arg;
        }
        if (arg == argc) return diagnose(*config, pattern, mode);
        std::cerr << "usage: dmf_paper_parallel_experiments [--groups N] --diagnose [pattern] [after_batch|static_batch]\n";
        return 2;
    }
    const bool compare_static = arg < argc && std::string_view(argv[arg]) == "--compare";
    const bool all_scenarios = compare_static ||
        (arg < argc && std::string_view(argv[arg]) == "--all");
    if (all_scenarios) ++arg;
    if (argc - arg > 1 || (arg < argc && std::string_view(argv[arg]).substr(0, 2) == "--")) {
        std::cerr << "usage: dmf_paper_parallel_experiments [--groups N] [--all|--compare] [output.csv]\n";
        return 2;
    }
    std::string prefix = "paper_parallel_";
    if (config->groups != group_count) prefix += std::to_string(config->groups) + '_';
    const std::string path = arg < argc ? argv[arg] :
        prefix + (compare_static ? "comparison.csv" : all_scenarios ? "trials.csv" : "scenario1.csv");
    std::ofstream output(path);
    if (!output) {
        std::cerr << "cannot create " << path << '\n';
        return 1;
    }
    const auto counts = runExperiments(output, *config, !all_scenarios, compare_static);
    if (!output) {
        std::cerr << "failed to write " << path << '\n';
        return 1;
    }
    std::cout << "wrote " << path << "; complete=" << counts.complete
              << ", timeout=" << counts.timeout
              << ", failed=" << counts.failed << '\n';
    return counts.timeout == 0 && counts.failed == 0 ? 0 : 1;
}
