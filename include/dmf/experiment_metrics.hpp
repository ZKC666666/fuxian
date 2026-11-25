#pragma once

#include "dmf/execution_loop.hpp"

#include <stdexcept>

namespace dmf {

class ExperimentTimeout : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct ExperimentSummary {
    int steps = 0;
    int route_requests = 0;
    int route_arrivals = 0;
    int output_wait_steps = 0;
    long long routing_microseconds = 0;
    long long routing_peak_microseconds = 0;
    long long loop_microseconds = 0;
    long long loop_peak_microseconds = 0;
    long long batch_planning_microseconds = 0;
    long long replay_microseconds = 0;
    int batch_plans = 0;
    std::size_t planned_route_steps = 0;
    int replayed_route_steps = 0;

    double routingThroughput() const;
};

// Collects one trial after each completed control step; rejects invalid state.
class ExperimentMetrics {
public:
    ExperimentMetrics(int width, int height, int minimum_separation);
    void observe(const StepReport& report, const DropletPositions& positions,
                 long long loop_microseconds);
    const ExperimentSummary& summary() const { return summary_; }

private:
    int width_;
    int height_;
    int minimum_separation_;
    ExperimentSummary summary_;
};

}  // namespace dmf
