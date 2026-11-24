#include "dmf/experiment_metrics.hpp"

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <stdexcept>

namespace dmf {

double ExperimentSummary::routingThroughput() const {
    return steps == 0 ? 0.0 : static_cast<double>(route_arrivals) / steps;
}

ExperimentMetrics::ExperimentMetrics(int width, int height, int minimum_separation)
    : width_(width), height_(height), minimum_separation_(minimum_separation) {
    if (width < 1 || height < 1 || minimum_separation < 1) {
        throw std::invalid_argument("invalid experiment geometry");
    }
}

void ExperimentMetrics::observe(const StepReport& report,
                                const DropletPositions& positions,
                                long long loop_microseconds) {
    if (report.step != summary_.steps + 1 || loop_microseconds < 0 ||
        report.routing_microseconds < 0) {
        throw std::logic_error("invalid experiment step or timing");
    }
    for (auto it = positions.begin(); it != positions.end(); ++it) {
        const auto p = it->second;
        if (p.x < 0 || p.y < 0 || p.x >= width_ || p.y >= height_) {
            throw std::logic_error("droplet outside chip");
        }
    }
    for (auto it = positions.begin(); it != positions.end(); ++it) {
        const auto p = it->second;
        for (auto other = std::next(it); other != positions.end(); ++other) {
            const auto dx = std::abs(static_cast<long long>(p.x) - other->second.x);
            const auto dy = std::abs(static_cast<long long>(p.y) - other->second.y);
            if (std::max(dx, dy) < minimum_separation_) {
                throw std::logic_error("droplet separation violated");
            }
        }
    }
    if (summary_.route_arrivals + report.route_arrivals.size() >
        summary_.route_requests + report.routing_tasks.size()) {
        throw std::logic_error("routing arrival has no matching request");
    }
    ++summary_.steps;
    summary_.route_requests += static_cast<int>(report.routing_tasks.size());
    summary_.route_arrivals += static_cast<int>(report.route_arrivals.size());
    if (!report.waiting_for_output_space.empty()) ++summary_.output_wait_steps;
    summary_.routing_microseconds += report.routing_microseconds;
    summary_.routing_peak_microseconds =
        std::max(summary_.routing_peak_microseconds, report.routing_microseconds);
    summary_.loop_microseconds += loop_microseconds;
    summary_.loop_peak_microseconds =
        std::max(summary_.loop_peak_microseconds, loop_microseconds);
}

}  // namespace dmf
