#include "dmf/experiment_metrics.hpp"
#include "paper_parallel_case.hpp"
#include "paper_trial_config.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
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
using dmf::paper::TrialSettings;
using dmf::paper::CalibrationCase;
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

struct TrialMilestones {
    int first_detect = 0;
    int last_detect = 0;
    int first_output_move = 0;
    int last_output_arrival = 0;
    int completed_detections = 0;
    int output_arrivals = 0;
    std::set<dmf::DropletId> output_droplets;

    void observe(const dmf::StepReport& report, const dmf::DynamicDag& dag) {
        for (const auto op : report.completed) {
            if (dag.operationType(op) != dmf::OperationType::Detect) continue;
            if (first_detect == 0) first_detect = report.step;
            last_detect = report.step;
            ++completed_detections;
        }
        for (const auto& task : report.routing_tasks) {
            if (dag.operationType(task.operation) == dmf::OperationType::Output) {
                output_droplets.insert(task.droplet);
            }
        }
        if (first_output_move == 0) {
            for (std::size_t i = 0; i < report.moves.size(); ++i) {
                const auto& move = report.moves[i];
                if (output_droplets.count(move.droplet) != 0 &&
                    !(report.observed.at(i).observed == move.from)) {
                    first_output_move = report.step;
                    break;
                }
            }
        }
        for (const auto& arrival : report.route_arrivals) {
            if (output_droplets.erase(arrival.droplet) != 0) {
                last_output_arrival = report.step;
                ++output_arrivals;
            }
        }
    }
};

struct TrialResult {
    dmf::ExperimentSummary metrics;
    int detour_searches;
    int synchronous_field_refreshes;
    std::string timeout;
    std::string failure;
    TrialMilestones milestones;
};

TrialResult runTrial(const ParallelCase& config, const std::vector<std::string>& outcomes,
                     dmf::BranchActivationMode mode,
                     dmf::CostFieldMode field_mode = dmf::CostFieldMode::Asynchronous,
                     std::ostream* trace = nullptr,
                     const TrialSettings* override_settings = nullptr) {
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
    const TrialSettings defaults{config.outputPort()};
    const auto& settings = override_settings ? *override_settings : defaults;
    dmf::UpperLayer upper(
        dag, chip,
        {{dmf::OperationType::Dispense, {1, 1, 1}},
         {dmf::OperationType::Mix, {3, 3, settings.mix_cycles}},
         {dmf::OperationType::Detect, {3, 3, settings.detect_cycles}}},
        positions,
        {{dmf::OperationType::Output, settings.output_port},
         {dmf::OperationType::Waste, config.wastePort()}},
        settings.inlets ? *settings.inlets : sourcePorts(config));
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
    TrialMilestones milestones;
    std::string failure;

    using Clock = std::chrono::steady_clock;
    for (int step = 0; step < max_steps && !loop.allDone(); ++step) {
        const auto start = Clock::now();
        dmf::StepReport report;
        try {
            report = loop.step(trace ? &decision : nullptr);
        } catch (const std::exception& error) {
            if (!override_settings) throw;
            failure = error.what();
            break;
        }
        const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now() - start).count();
        metrics.observe(report, positions, micros);
        if (override_settings) milestones.observe(report, dag);
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
                       router.synchronousFieldRefreshes(), {}, std::move(failure),
                       std::move(milestones)};
    if (!result.failure.empty()) return result;
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

void checkOutputSeparation(const ParallelCase& config, dmf::Point output) {
    for (const auto& entry : sourcePorts(config)) {
        const auto inlet = entry.second;
        if (std::max(std::abs(output.x - inlet.x), std::abs(output.y - inlet.y)) < 2) {
            throw std::logic_error("sensitivity output port is too close to an inlet");
        }
    }
}

void checkTrialTimeline(const TrialResult& trial, int groups, int outputs,
                        dmf::BranchActivationMode mode) {
    const auto& time = trial.milestones;
    const int release_after = mode == dmf::BranchActivationMode::StaticBatch ?
        time.last_detect : time.first_detect;
    if (time.completed_detections != groups || time.output_arrivals != outputs ||
        time.first_detect <= 0 || time.last_detect < time.first_detect) {
        throw std::logic_error("trial timeline has missing detections or output arrivals");
    }
    if (outputs > 0 &&
        (time.first_output_move <= release_after ||
         time.last_output_arrival < time.first_output_move ||
         time.last_output_arrival >= trial.metrics.steps ||
         (outputs == groups && time.last_output_arrival + 1 != trial.metrics.steps))) {
        throw std::logic_error("trial timeline has out-of-order output events");
    }
}

int runParameterTrials(const std::string& path, const std::vector<CalibrationCase>& profiles,
                       int repeats) {
    std::ofstream out(path);
    if (!out) {
        std::cerr << "cannot create " << path << '\n';
        return 1;
    }
    out << "profile,repeat,chip_side,groups,output_x,output_y,mix_cycles,detect_cycles,mode,status,"
           "steps,first_detect_step,last_detect_step,first_output_move_step,last_output_arrival_step,"
           "route_requests,route_arrivals,error\n";
    RunCounts counts;
    for (const auto& profile : profiles) {
        const auto& config = profile.chip;
        std::vector<std::string> outcomes;
        for (const char branch : profile.branches) {
            outcomes.emplace_back(branch == 'O' ? "output" : "waste");
        }
        const int outputs = static_cast<int>(std::count(profile.branches.begin(),
                                                       profile.branches.end(), 'O'));
        for (int repeat = 1; repeat <= repeats; ++repeat) {
            for (const auto mode : {dmf::BranchActivationMode::Immediate,
                                    dmf::BranchActivationMode::StaticBatch}) {
                const auto& s = profile.settings;
                out << profile.name << ',' << repeat << ',' << config.chip_side << ',' << config.groups
                    << ',' << s.output_port.x << ',' << s.output_port.y << ',' << s.mix_cycles
                    << ',' << s.detect_cycles << ',' << modeName(mode) << ',';
                try {
                    const auto trial = runTrial(config, outcomes, mode, dmf::CostFieldMode::Asynchronous,
                                                nullptr, &s);
                    const char* status = "complete";
                    if (!trial.failure.empty()) {
                        status = "failed";
                        ++counts.failed;
                    } else if (!trial.timeout.empty()) {
                        status = "timeout";
                        ++counts.timeout;
                    } else {
                        checkTrialTimeline(trial, config.groups, outputs, mode);
                        ++counts.complete;
                    }
                    const auto& time = trial.milestones;
                    out << status << ','
                        << trial.metrics.steps << ',' << time.first_detect << ',' << time.last_detect
                        << ',' << time.first_output_move << ',' << time.last_output_arrival
                        << ',' << trial.metrics.route_requests << ',' << trial.metrics.route_arrivals
                        << ',' << csvText(trial.failure.empty() ? trial.timeout : trial.failure) << '\n';
                } catch (const std::exception& error) {
                    ++counts.failed;
                    out << "failed,,,,,,,," << csvText(error.what()) << '\n';
                }
            }
        }
    }
    out.close();
    if (!out) {
        std::cerr << "cannot write " << path << '\n';
        return 1;
    }
    std::cout << "wrote " << path << "; complete=" << counts.complete
              << ", timeout=" << counts.timeout << ", failed=" << counts.failed << '\n';
    return counts.timeout == 0 && counts.failed == 0 ? 0 : 1;
}

int runSensitivity(const std::string& path) {
    const auto config = *dmf::paper::caseForGroups(25);
    const std::string branches(config.groups, 'O');
    const std::vector<CalibrationCase> profiles{
        {"reference", config, {config.outputPort()}, branches},
        {"output_middle", config, {{59, 29}}, branches},
        {"mix_longer", config, {config.outputPort(), 6, 2}, branches},
        {"detect_longer", config, {config.outputPort(), 3, 4}, branches}
    };
    for (const auto& profile : profiles) checkOutputSeparation(config, profile.settings.output_port);
    return runParameterTrials(path, profiles, 5);
}

int runCalibration(int argc, char** argv) {
    if (argc != 4 && argc != 5) {
        std::cerr << "usage: dmf_paper_parallel_experiments --calibrate cases.txt results.csv [repeats]\n";
        return 2;
    }
    int repeats = 5;
    if (argc == 5) {
        const std::string_view value(argv[4]);
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), repeats);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
            repeats < 1 || repeats > 50) {
            std::cerr << "calibration repeats must be between 1 and 50\n";
            return 2;
        }
    }
    try {
        const auto cases = dmf::paper::loadCalibrationCases(argv[2]);
        return runParameterTrials(argv[3], cases, repeats);
    } catch (const std::exception& error) {
        std::cerr << "calibration: " << error.what() << '\n';
        return 2;
    }
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
    if (argc >= 2 && std::string_view(argv[1]) == "--calibrate") return runCalibration(argc, argv);
    if (argc == 2 && std::string_view(argv[1]) == "--check-1601") return checkSeed1601();
    if (argc >= 2 && std::string_view(argv[1]) == "--sensitivity") {
        if (argc <= 3) return runSensitivity(argc == 3 ? argv[2] : "paper_parallel_sensitivity.csv");
        std::cerr << "usage: dmf_paper_parallel_experiments --sensitivity [output.csv]\n";
        return 2;
    }
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
