#include "dmf/execution_loop.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>

namespace dmf {

namespace {
using Clock = std::chrono::steady_clock;
long long elapsedMicroseconds(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
}
}  // namespace

ExecutionLoop::ExecutionLoop(DynamicDag& dag, UpperLayer& upper, Router& router,
                             CompletionFeedbackProvider completion_feedback_provider,
                             MovementFeedbackProvider movement_provider)
    : dag_(dag), upper_(upper), router_(router),
      completion_feedback_provider_(std::move(completion_feedback_provider)),
      movement_provider_(std::move(movement_provider)) {
    if (!completion_feedback_provider_) {
        throw std::invalid_argument("completion feedback provider is empty");
    }
}

void ExecutionLoop::recordStart(OperationInstanceId operation, StepReport& report) {
    if (!remaining_cycles_.emplace(operation, upper_.durationFor(operation)).second) {
        throw std::logic_error("operation started twice");
    }
    report.started.push_back(operation);
}

void ExecutionLoop::acceptArrivals(const std::vector<RouteArrival>& arrivals,
                                   StepReport& report) {
    for (const auto& arrival : arrivals) {
        if (upper_.reportArrival(arrival.operation, arrival.droplet)) {
            recordStart(arrival.operation, report);
        }
    }
}

void ExecutionLoop::acceptStarts(const std::vector<OperationInstanceId>& started,
                                 StepReport& report) {
    for (const auto operation : started) recordStart(operation, report);
}

void ExecutionLoop::updateBlockages(StepReport& report) {
    const auto start = Clock::now();
    router_.setBlockages(upper_.activeBlockages());
    report.blockage_update_microseconds += elapsedMicroseconds(start);
}

StepReport ExecutionLoop::step() {
    StepReport report;
    report.step = ++step_count_;
    const int fields_before = router_.costFieldBuilds();
    dag_.advanceCycle();  // Outputs and branches from earlier steps become available.
    const auto running_before = remaining_cycles_;

    acceptArrivals(router_.addTasks(upper_.placeReadyOperations()), report);
    acceptStarts(upper_.tryStartPlacedOperations(), report);
    updateBlockages(report);

    if (router_.hasTasks()) {
        const auto routing_start = Clock::now();
        report.moves = router_.planStep();
        report.routing_microseconds = elapsedMicroseconds(routing_start);
        report.observed.reserve(report.moves.size());
        for (const auto& move : report.moves) {
            const auto position = movement_provider_
                                      ? movement_provider_(move, report.step)
                                      : move.to;
            report.observed.push_back({move.droplet, position});
        }
        acceptArrivals(router_.applyFeedback(report.observed), report);
        acceptStarts(upper_.tryStartPlacedOperations(), report);
    }

    for (const auto& [operation, cycles] : running_before) {
        if (cycles > 1) {
            --remaining_cycles_.at(operation);
            continue;
        }
        auto feedback = completion_feedback_provider_(operation);
        upper_.finishOperation(operation, std::move(feedback.outputs),
                               std::move(feedback.outcome));
        remaining_cycles_.erase(operation);
        report.completed.push_back(operation);
    }
    updateBlockages(report);
    report.active_blockages = upper_.activeBlockages().size();
    report.new_cost_fields = router_.costFieldBuilds() - fields_before;
    return report;
}

void ExecutionLoop::completeEarly(OperationInstanceId operation,
                                  CompletionFeedback feedback) {
    if (remaining_cycles_.count(operation) == 0) {
        throw std::logic_error("operation is not running");
    }
    upper_.finishOperation(operation, std::move(feedback.outputs),
                           std::move(feedback.outcome));
    remaining_cycles_.erase(operation);
    router_.setBlockages(upper_.activeBlockages());
}

bool ExecutionLoop::allDone() const {
    return dag_.allDone() && remaining_cycles_.empty() && !router_.hasTasks();
}

int ExecutionLoop::remainingCycles(OperationInstanceId operation) const {
    return remaining_cycles_.at(operation);
}

}  // namespace dmf
