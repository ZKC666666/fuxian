#include "parallel_protocol_demo.hpp"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

std::string csvText(const std::string& value) {
    std::string quoted = "\"";
    for (const char c : value) {
        if (c == '"') quoted += '"';
        quoted += c;
    }
    return quoted + '"';
}

struct Totals {
    int successes = 0;
    int failures = 0;
    int min_steps = 0;
    int max_steps = 0;
    long long steps = 0;
    long long arrivals = 0;
    long long routing_microseconds = 0;
};

void writeResults(const std::string& trial_path,
                  const std::string& summary_path) {
    std::ofstream trials(trial_path);
    std::ofstream summaries(summary_path);
    if (!trials || !summaries) {
        throw std::runtime_error("cannot create experiment CSV files");
    }
    trials << "case,mode,repeat,chip,seed,source,complete,steps,route_requests,"
              "route_arrivals,throughput,plan_step_total_us,plan_step_peak_us,"
              "loop_total_us,loop_peak_us,output_wait_steps,error\n";
    summaries << "case,mode,successes,failures,mean_steps,min_steps,max_steps,"
                 "route_arrivals_per_step,mean_plan_step_us_per_cycle\n";
    trials << std::fixed << std::setprecision(6);
    summaries << std::fixed << std::setprecision(6);

    for (const std::string& first : {"output", "waste"}) {
        for (const std::string& second : {"output", "waste"}) {
            const std::string name = first + "_" + second;
            for (const auto mode : {dmf::BranchActivationMode::Immediate,
                                    dmf::BranchActivationMode::AfterBatch}) {
                const std::string mode_name = mode == dmf::BranchActivationMode::Immediate
                                                  ? "immediate" : "after_batch";
                Totals totals;
                for (int repeat = 1; repeat <= 5; ++repeat) {
                    trials << name << ',' << mode_name << ',' << repeat
                           << ",27x15,none,fixed-v1.1,";
                    try {
                        const auto result = dmf::demo::runParallelProtocol(
                            first, second, false, false, mode);
                        trials << "true," << result.steps << ','
                               << result.route_requests << ',' << result.route_arrivals
                               << ',' << result.routingThroughput() << ','
                               << result.routing_microseconds << ','
                               << result.routing_peak_microseconds << ','
                               << result.loop_microseconds << ','
                               << result.loop_peak_microseconds << ','
                               << result.output_wait_steps << ",\"\"\n";
                        ++totals.successes;
                        totals.steps += result.steps;
                        totals.arrivals += result.route_arrivals;
                        totals.routing_microseconds += result.routing_microseconds;
                        if (totals.successes == 1 || result.steps < totals.min_steps) {
                            totals.min_steps = result.steps;
                        }
                        if (result.steps > totals.max_steps) totals.max_steps = result.steps;
                    } catch (const dmf::ExperimentTimeout& error) {
                        ++totals.failures;
                        trials << "timeout,,,,,,,,,," << csvText(error.what()) << '\n';
                    } catch (const std::exception& error) {
                        ++totals.failures;
                        trials << "failed,,,,,,,,,," << csvText(error.what()) << '\n';
                    }
                }
                summaries << name << ',' << mode_name << ',' << totals.successes
                          << ',' << totals.failures << ',';
                if (totals.successes > 0) {
                    summaries << static_cast<double>(totals.steps) / totals.successes << ','
                              << totals.min_steps << ',' << totals.max_steps << ','
                              << static_cast<double>(totals.arrivals) / totals.steps << ','
                              << static_cast<double>(totals.routing_microseconds) /
                                     totals.steps;
                } else {
                    summaries << ",,,,";
                }
                summaries << '\n';
            }
        }
    }
    if (!trials || !summaries) throw std::runtime_error("failed to write experiment CSV");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 1 && argc != 3) {
        std::cerr << "usage: dmf_parallel_experiments [trials.csv summary.csv]\n";
        return 2;
    }
    const std::string trial_path = argc == 3 ? argv[1] : "experiment_trials.csv";
    const std::string summary_path = argc == 3 ? argv[2] : "experiment_summary.csv";
    if (trial_path == summary_path) {
        std::cerr << "trial and summary paths must differ\n";
        return 2;
    }
    try {
        writeResults(trial_path, summary_path);
        std::cout << "wrote " << trial_path << " and " << summary_path << '\n';
    } catch (const std::exception& error) {
        std::cerr << "experiment: " << error.what() << '\n';
        return 1;
    }
}
