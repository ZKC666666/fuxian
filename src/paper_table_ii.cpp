#include "dmf/execution_loop.hpp"
#include "dmf/experiment_metrics.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace dmf;

struct Configuration {
    int chip_side;
    int samples;
    int controls;
    int conditions;

    int sites() const { return (samples + controls) * conditions; }
    int modulesPerSide() const { return chip_side == 30 ? 4 : 9; }
    std::string name() const {
        return "N" + std::to_string(samples) + "_C" + std::to_string(controls) +
               "_M" + std::to_string(conditions);
    }
};

constexpr std::array<Configuration, 6> configurations{{
    {30, 2, 1, 4}, {30, 2, 2, 3}, {30, 3, 1, 3},
    {60, 3, 1, 5}, {60, 4, 2, 6}, {60, 5, 2, 7}
}};
constexpr std::array<int, 4> pass_rates{100, 90, 80, 70};
constexpr std::array<const char*, 4> paths{"Short", "Medium-1", "Medium-2", "Long"};
constexpr int seeds = 5;
constexpr int max_steps = 1000;

std::string siteName(const char* operation, int site) {
    return std::string(operation) + '_' + std::to_string(site);
}

void addPreparation(BlockSpec& block, int site) {
    const auto a = siteName("dispense_a", site);
    const auto b = siteName("dispense_b", site);
    block.operations.push_back({a, OperationType::Dispense, {}, 1});
    block.operations.push_back({b, OperationType::Dispense, {}, 1});
    block.operations.push_back({siteName("check", site), OperationType::Detect,
                                {InputRef::output(a), InputRef::output(b)}, 2});
}

Protocol makeProtocol(int site_count) {
    BlockSpec start{"start", 0, {}, {}};
    BlockSpec merge{"merge", static_cast<std::size_t>(2 * site_count), {}, {}};
    BlockSpec discard{"discard", 2, {{"waste_a", OperationType::Waste, {InputRef::entry(0)}, 0},
                                     {"waste_b", OperationType::Waste, {InputRef::entry(1)}, 0}}, {}};
    Protocol protocol{"start", {{discard.id, discard}}};
    for (int site = 0; site < site_count; ++site) {
        addPreparation(start, site);
        BlockSpec retry{siteName("retry", site), 0, {}, {}};
        addPreparation(retry, site);
        const auto id = retry.id;
        protocol.blocks.emplace(id, std::move(retry));
        const auto a = static_cast<std::size_t>(2 * site);
        merge.operations.push_back({siteName("mix", site), OperationType::Mix,
                                    {InputRef::entry(a), InputRef::entry(a + 1)}, 1});
    }
    protocol.blocks.emplace("start", std::move(start));
    protocol.blocks.emplace("merge", std::move(merge));
    return protocol;
}

std::map<std::string, Point> inletPositions(const Configuration& config) {
    std::map<std::string, Point> ports;
    const int edge = config.chip_side - 1;
    const int pair_slots = (edge - 5) / 4 + 1;
    for (int site = 0; site < config.sites(); ++site) {
        const int side = site % 4;
        const int on_side = (config.sites() + 3 - side) / 4;
        const int slot = (2 * (site / 4) + 1) * pair_slots / (2 * on_side);
        const int first = 3 + 4 * slot;
        const auto point = [side, edge](int along) {
            switch (side) {
                case 0: return Point{0, along};
                case 1: return Point{edge, along};
                case 2: return Point{along, 0};
                default: return Point{along, edge};
            }
        };
        ports.emplace(siteName("dispense_a", site), point(first));
        ports.emplace(siteName("dispense_b", site), point(first + 2));
    }
    return ports;
}

std::string firstCheckOutcomes(const Configuration& config, int pass_rate, unsigned seed) {
    const int count = config.sites();
    const int rejected = (count * (100 - pass_rate) + 50) / 100;
    std::vector<int> order(count);
    std::iota(order.begin(), order.end(), 0);
    std::mt19937 engine(seed);
    std::shuffle(order.begin(), order.end(), engine);
    std::string outcomes(count, 'P');
    for (int i = 0; i < rejected; ++i) outcomes.at(order[i]) = 'F';
    return outcomes;
}

enum class SiteStage { Checking, Discarding, Accepted };

struct Site {
    BlockInstanceId block;
    SiteStage stage = SiteStage::Checking;
    int attempt = 1;
    std::vector<DropletId> accepted;
};

struct Result {
    int steps = 0;
    int checks = 0;
    int retries = 0;
    int discarded = 0;
    int last_accept_step = 0;
    int mix_start_step = 0;
    int mixed = 0;
    int batch_plans = 0;
    double loop_ms = 0;
    std::string status = "timeout";
    std::string error;
};

struct PlannedStep {
    std::vector<MoveCommand> moves;
    std::vector<DropletId> consumed;
    DropletPositions produced;
};

PlannedStep captureStep(const StepReport& report, const DynamicDag& dag,
                        const DropletPositions& positions) {
    PlannedStep step{report.moves, {}, {}};
    for (const auto op : report.completed) {
        const auto& operation = dag.operation(op);
        step.consumed.insert(step.consumed.end(), operation.inputs.begin(), operation.inputs.end());
        for (const auto drop : operation.outputs) step.produced.emplace(drop, positions.at(drop));
    }
    return step;
}

void replayBatch(const std::vector<PlannedStep>& plan, DropletPositions& positions) {
    for (const auto& step : plan) {
        for (const auto& move : step.moves) {
            if (!(positions.at(move.droplet) == move.from)) {
                throw std::logic_error("batch replay did not match planned start position");
            }
            positions.at(move.droplet) = move.to;
        }
        for (const auto drop : step.consumed) {
            if (positions.erase(drop) != 1) {
                throw std::logic_error("batch replay consumed a missing droplet");
            }
        }
        for (const auto& [drop, point] : step.produced) {
            if (!positions.emplace(drop, point).second) {
                throw std::logic_error("batch replay produced a duplicate droplet");
            }
        }
    }
}

bool complete(const DynamicDag& dag, BlockInstanceId block, const std::string& id) {
    return dag.operation(dag.operationFor(block, id)).state == OperationState::Completed;
}

void advanceSites(DynamicDag& dag, std::vector<Site>& sites, std::string_view outcomes,
                  Result& result) {
    for (std::size_t index = 0; index < sites.size(); ++index) {
        auto& site = sites[index];
        if (site.stage == SiteStage::Checking) {
            const auto& check = dag.operation(dag.operationFor(site.block, siteName("check", static_cast<int>(index))));
            if (check.state != OperationState::Completed) continue;
            ++result.checks;
            if (site.attempt == 1 && outcomes[index] == 'F') {
                site.block = dag.activateReadyBlock("discard", check.outputs);
                site.stage = SiteStage::Discarding;
            } else {
                site.accepted = check.outputs;
                site.stage = SiteStage::Accepted;
                result.last_accept_step = result.steps;
            }
        } else if (site.stage == SiteStage::Discarding &&
                   complete(dag, site.block, "waste_a") && complete(dag, site.block, "waste_b")) {
            result.discarded += 2;
            ++result.retries;
            site.block = dag.activateReadyBlock(siteName("retry", static_cast<int>(index)), {});
            site.stage = SiteStage::Checking;
            ++site.attempt;
        }
    }
}

Result runTrial(const Configuration& config, const std::string& outcomes, bool batch) {
    const int site_count = config.sites();
    const int edge = config.chip_side - 1;
    DynamicDag dag(makeProtocol(site_count));
    const auto start_block = dag.activateStart({});
    std::vector<Site> sites(site_count, Site{start_block, SiteStage::Checking, 1, {}});
    DropletPositions positions;
    VirtualChip chip(config.modulesPerSide(), config.modulesPerSide(), 3,
                     config.chip_side, config.chip_side);
    UpperLayer upper(dag, std::move(chip),
        {{OperationType::Dispense, {1, 1, 1}}, {OperationType::Detect, {3, 3, 8}},
         {OperationType::Mix, {3, 3, 29}}},
        positions, {{OperationType::Waste, {0, edge}}}, inletPositions(config));
    Router router(config.chip_side, config.chip_side, positions);
    DropletId next_droplet = 1;
    ExecutionLoop loop(dag, upper, router, [&](OperationInstanceId operation) {
        switch (dag.operationType(operation)) {
            case OperationType::Dispense:
            case OperationType::Mix: return CompletionFeedback{{next_droplet++}, std::nullopt};
            case OperationType::Detect: return CompletionFeedback{{next_droplet++, next_droplet++}, std::nullopt};
            default: return CompletionFeedback{{}, std::nullopt};
        }
    });
    ExperimentMetrics metrics(config.chip_side, config.chip_side, router.minimumSeparation());

    std::vector<PlannedStep> plan;
    DropletPositions execution_positions;
    bool merge_activated = false;
    Result result;
    using Clock = std::chrono::steady_clock;
    try {
        while (result.steps < max_steps) {
            const auto started = Clock::now();
            const auto report = loop.step();
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                Clock::now() - started).count();
            result.steps = report.step;
            result.loop_ms += static_cast<double>(elapsed) / 1000.0;
            metrics.observe(report, positions, elapsed);
            const auto started_mixes = std::count_if(report.started.begin(), report.started.end(),
                [&](const auto op) { return dag.operationType(op) == OperationType::Mix; });
            if (started_mixes != 0) {
                if (started_mixes != site_count || result.mix_start_step != 0 ||
                    result.last_accept_step >= result.steps) {
                    throw std::logic_error("final Mix operations did not start together after all checks");
                }
                result.mix_start_step = result.steps;
            }
            for (const auto op : report.completed) {
                if (dag.operationType(op) == OperationType::Mix) ++result.mixed;
            }

            if (batch) {
                plan.push_back(captureStep(report, dag, positions));
                if (!dag.activatedOperationsComplete()) continue;
                replayBatch(plan, execution_positions);
                if (execution_positions != positions) {
                    throw std::logic_error("batch replay end positions diverged from its plan");
                }
                plan.clear();
                ++result.batch_plans;
            }
            if (merge_activated) {
                if (loop.allDone()) break;
                continue;
            }
            advanceSites(dag, sites, outcomes, result);
            if (!std::all_of(sites.begin(), sites.end(), [](const Site& site) {
                    return site.stage == SiteStage::Accepted;
                })) continue;
            std::vector<DropletId> inputs;
            for (const auto& site : sites) inputs.insert(inputs.end(), site.accepted.begin(), site.accepted.end());
            dag.activateReadyBlock("merge", std::move(inputs));
            merge_activated = true;
        }
        if (merge_activated && loop.allDone()) {
            const int failures = static_cast<int>(std::count(outcomes.begin(), outcomes.end(), 'F'));
            const auto counts = metrics.summary();
            const int expected_routes = 4 * (site_count + failures);
            const int expected_batches = batch ? (failures == 0 ? 2 : 4) : 0;
            if (result.checks != site_count + failures || result.retries != failures ||
                result.discarded != 2 * failures || result.mix_start_step == 0 ||
                result.mixed != site_count || result.batch_plans != expected_batches ||
                positions.size() != static_cast<std::size_t>(site_count) ||
                counts.route_requests != expected_routes || counts.route_arrivals != expected_routes) {
                throw std::logic_error("completed trial has inconsistent operation or route counts");
            }
            result.status = "complete";
        } else {
            result.error = "step limit reached";
        }
    } catch (const std::exception& error) {
        result.status = "failed";
        result.error = error.what();
    }
    return result;
}

std::string csvQuote(std::string_view input) {
    std::string quoted = "\"";
    for (const auto c : input) {
        if (c == '"') quoted += '"';
        quoted += c;
    }
    return quoted + '"';
}

int runExperiments(bool pilot, const std::string& filename) {
    std::ofstream out(filename);
    if (!out) throw std::runtime_error("cannot write " + filename);
    out << "config,chip_side,sites,path,first_pass_percent,seed,outcomes,mode,status,steps,"
           "checks,retries,discarded,last_accept_step,mix_start_step,mixed,batch_plans,"
           "avg_cpp_step_ms,error\n" << std::fixed << std::setprecision(6);
    int complete_trials = 0;
    int total_trials = 0;
    const auto config_count = pilot ? 1u : configurations.size();
    for (std::size_t i = 0; i < config_count; ++i) {
        const auto& config = configurations[i];
        for (std::size_t scenario = 0; scenario < paths.size(); ++scenario) {
            if (pilot && scenario != 0 && scenario + 1 != paths.size()) continue;
            for (unsigned seed = 6201; seed < 6201 + seeds; ++seed) {
                const auto outcomes = firstCheckOutcomes(config, pass_rates[scenario], seed);
                for (const bool batch : {false, true}) {
                    const auto run = runTrial(config, outcomes, batch);
                    ++total_trials;
                    if (run.status == "complete") ++complete_trials;
                    out << config.name() << ',' << config.chip_side << ',' << config.sites() << ','
                        << paths[scenario] << ',' << pass_rates[scenario] << ',' << seed << ','
                        << outcomes << ',' << (batch ? "batch_plan" : "immediate") << ','
                        << run.status << ',' << run.steps << ',' << run.checks << ',' << run.retries
                        << ',' << run.discarded << ',' << run.last_accept_step << ',' << run.mix_start_step
                        << ',' << run.mixed << ',' << run.batch_plans << ','
                        << run.loop_ms / std::max(run.steps, 1) << ',' << csvQuote(run.error) << '\n';
                }
            }
        }
    }
    out.close();
    if (!out) throw std::runtime_error("could not save " + filename);
    std::cout << "Table II " << (pilot ? "pilot" : "full run") << ": " << complete_trials << '/'
              << total_trials << " complete; " << filename << '\n';
    return complete_trials == total_trials ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string_view mode = argc > 1 ? argv[1] : "--pilot";
    if (argc > 3 || (mode != "--pilot" && mode != "--all")) {
        std::cerr << "usage: dmf_paper_table_ii [--pilot|--all] [results.csv]\n";
        return 2;
    }
    try {
        const bool pilot = mode == "--pilot";
        return runExperiments(pilot, argc == 3 ? argv[2] :
                              pilot ? "paper_table_ii_pilot.csv" : "paper_table_ii_trials.csv");
    } catch (const std::exception& error) {
        std::cerr << "Table II: " << error.what() << '\n';
        return 2;
    }
}
