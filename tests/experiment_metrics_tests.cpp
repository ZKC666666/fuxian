#include "dmf/experiment_metrics.hpp"

#include <stdexcept>
#include <string>

namespace {
void check(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

void countsArrivalsRatherThanRequests() {
    dmf::ExperimentMetrics metrics(5, 5, 2);
    dmf::DropletPositions positions{{1, {0, 0}}};
    dmf::StepReport first;
    first.step = 1;
    first.routing_tasks.push_back({1, 1, {4, 0}, 0});
    first.routing_microseconds = 7;
    metrics.observe(first, positions, 11);
    check(metrics.summary().route_arrivals == 0, "request counted as completion");

    positions.at(1) = {4, 0};
    dmf::StepReport second;
    second.step = 2;
    second.route_arrivals.push_back({1, 1});
    second.routing_microseconds = 9;
    metrics.observe(second, positions, 13);
    const auto result = metrics.summary();
    check(result.steps == 2 && result.route_requests == 1 &&
              result.route_arrivals == 1 && result.routing_microseconds == 16 &&
              result.loop_microseconds == 24,
          "trial totals are inconsistent");
}

void rejectsUnsafeOrRepeatedSteps() {
    dmf::ExperimentMetrics metrics(5, 5, 2);
    dmf::StepReport report;
    report.step = 1;
    dmf::DropletPositions positions{{1, {0, 0}}, {2, {1, 1}}};
    bool rejected = false;
    try { metrics.observe(report, positions, 1); }
    catch (const std::logic_error&) { rejected = true; }
    check(rejected && metrics.summary().steps == 0, "unsafe step was accepted");

    positions.at(2) = {2, 2};
    metrics.observe(report, positions, 1);
    rejected = false;
    try { metrics.observe(report, positions, 1); }
    catch (const std::logic_error&) { rejected = true; }
    check(rejected, "repeated step was accepted");
}

void rejectsUnknownArrivalAndOutOfBoundsDroplet() {
    dmf::ExperimentMetrics metrics(5, 5, 2);
    dmf::StepReport report;
    report.step = 1;
    report.route_arrivals.push_back({1, 1});
    dmf::DropletPositions positions{{1, {0, 0}}};
    bool rejected = false;
    try { metrics.observe(report, positions, 1); }
    catch (const std::logic_error&) { rejected = true; }
    check(rejected && metrics.summary().steps == 0,
          "arrival without request was accepted");

    report.route_arrivals.clear();
    positions.at(1) = {5, 0};
    rejected = false;
    try { metrics.observe(report, positions, 1); }
    catch (const std::logic_error&) { rejected = true; }
    check(rejected && metrics.summary().steps == 0,
          "out-of-bounds droplet was accepted");
}
}  // namespace

int main() {
    countsArrivalsRatherThanRequests();
    rejectsUnsafeOrRepeatedSteps();
    rejectsUnknownArrivalAndOutOfBoundsDroplet();
}
