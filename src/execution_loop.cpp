#include "dmf/execution_loop.hpp"

#include <chrono>
#include <set>
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
                             MovementFeedbackProvider movement_provider,
                             std::vector<VirtualModule> fixed_blockages,
                             BranchActivationMode branch_mode)
    : dag_(dag), upper_(upper), router_(router),
      completion_feedback_provider_(std::move(completion_feedback_provider)),
      movement_provider_(std::move(movement_provider)),
      fixed_blockages_(std::move(fixed_blockages)), branch_mode_(branch_mode) {
    if (!completion_feedback_provider_) {
        throw std::invalid_argument("completion feedback provider is empty");
    }
    if (upper_.minimumSeparation() != router_.minimumSeparation()) {
        throw std::invalid_argument("upper layer and router separation must match");
    }
    if (branch_mode_ == BranchActivationMode::StaticBatch && movement_provider_) {
        throw std::invalid_argument("static terminal batches require ideal movement feedback");
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
        report.route_arrivals.push_back(arrival);
        if (upper_.reportArrival(arrival.operation, arrival.droplet,
                                 router_.activationHolds())) {
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
    auto blockages = fixed_blockages_;
    const auto active = upper_.activeBlockages();
    blockages.insert(blockages.end(), active.begin(), active.end());
    router_.setBlockages(blockages);
    router_.setUpcomingBlockages(upper_.upcomingBlockages());
    report.blockage_update_microseconds += elapsedMicroseconds(start);
}

void ExecutionLoop::prepareCrossings(StepReport& report) {
    const auto start = Clock::now();
    router_.prepareUpcomingCrossings();
    report.connectivity_check_microseconds += elapsedMicroseconds(start);
}

void ExecutionLoop::prepareTerminalBatch(StepReport& report) {
    if (terminal_plan_ || router_.hasTasks() || !remaining_cycles_.empty() ||
        report.routing_tasks.size() != report.activated_blocks.size()) {
        throw std::logic_error("static mode requires one terminal operation per released branch");
    }
    std::set<BlockInstanceId> released(report.activated_blocks.begin(), report.activated_blocks.end());
    for (const auto& task : report.routing_tasks) {
        const auto type = dag_.operationType(task.operation);
        if ((type != OperationType::Output && type != OperationType::Waste) ||
            released.erase(dag_.operation(task.operation).block_instance) != 1) {
            throw std::logic_error("static mode only plans terminal Output and Waste branches");
        }
    }
    const auto start = Clock::now();
    terminal_plan_ = router_.planTerminalBatch(report.routing_tasks);
    report.batch_planning_microseconds = elapsedMicroseconds(start);
    report.batch_plans = 1;
    report.planned_route_steps = terminal_plan_->size();
}

StepReport ExecutionLoop::step(RouteDecisionTrace* trace) {
    StepReport report;
    report.step = ++step_count_;
    const int fields_before = router_.costFieldBuilds();
    const int swaps_before = router_.asyncFieldSwaps();
    const int stale_before = router_.staleFieldDecisions();
    const bool release_branches = branch_mode_ == BranchActivationMode::Immediate ||
                                  dag_.activatedOperationsComplete();
    report.activated_blocks = dag_.advanceCycle(release_branches);
    const auto running_before = remaining_cycles_;

    report.routing_tasks = upper_.placeReadyOperations();
    if (branch_mode_ == BranchActivationMode::StaticBatch && !report.activated_blocks.empty()) {
        prepareTerminalBatch(report);
    }
    const auto immediate_arrivals = router_.addTasks(report.routing_tasks);
    updateBlockages(report);
    prepareCrossings(report);
    acceptArrivals(immediate_arrivals, report);
    acceptStarts(upper_.tryStartPlacedOperations(router_.activationHolds()), report);
    updateBlockages(report);
    prepareCrossings(report);

    if (router_.hasTasks()) {
        const auto routing_start = Clock::now();
        if (terminal_plan_) {
            report.moves = terminal_plan_->at(next_planned_step_);
            router_.replayTerminalStep(report.moves);
            ++next_planned_step_;
            report.replayed_route_steps = 1;
            report.replay_microseconds = elapsedMicroseconds(routing_start);
            if (trace) *trace = RouteDecisionTrace{trace->droplet};
        } else {
            report.moves = router_.planStep(trace);
            report.routing_microseconds = elapsedMicroseconds(routing_start);
        }
        report.evacuation_targets = router_.evacuationTargets();
        report.crossing_modules = router_.crossingModules();
        report.waiting_for_crossing = router_.waitingForCrossing();
        report.held_modules.assign(router_.activationHolds().begin(),
                                   router_.activationHolds().end());
        report.waiting_for_evacuation = router_.waitingForEvacuation();
        report.observed.reserve(report.moves.size());
        for (const auto& move : report.moves) {
            const auto position = movement_provider_
                                      ? movement_provider_(move, report.step)
                                      : move.to;
            report.observed.push_back({move.droplet, position});
        }
        const auto arrivals = router_.applyFeedback(report.observed);
        if (terminal_plan_ && !router_.hasTasks() && next_planned_step_ != terminal_plan_->size()) {
            throw std::logic_error("terminal plan contains unused steps");
        }
        prepareCrossings(report);
        acceptArrivals(arrivals, report);
        report.evacuated = router_.evacuatedLastStep();
        acceptStarts(upper_.tryStartPlacedOperations(router_.activationHolds()), report);
    } else if (trace) {
        *trace = RouteDecisionTrace{trace->droplet};
    }

    for (const auto& [operation, cycles] : running_before) {
        if (cycles > 1) {
            --remaining_cycles_.at(operation);
            continue;
        }
        auto feedback = pending_completion_.find(operation);
        if (feedback == pending_completion_.end()) {
            feedback = pending_completion_
                           .emplace(operation, completion_feedback_provider_(operation))
                           .first;
        }
        if (!upper_.canFinishOperation(operation, feedback->second.outputs)) {
            report.waiting_for_output_space.push_back(operation);
            continue;
        }
        upper_.finishOperation(operation, feedback->second.outputs,
                               feedback->second.outcome);
        pending_completion_.erase(feedback);
        remaining_cycles_.erase(operation);
        report.completed.push_back(operation);
    }
    updateBlockages(report);
    report.active_blockages = upper_.activeBlockages().size();
    report.new_cost_fields = router_.costFieldBuilds() - fields_before;
    report.async_field_swaps = router_.asyncFieldSwaps() - swaps_before;
    report.stale_field_decisions = router_.staleFieldDecisions() - stale_before;
    return report;
}

void ExecutionLoop::completeEarly(OperationInstanceId operation,
                                  CompletionFeedback feedback) {
    if (remaining_cycles_.count(operation) == 0) {
        throw std::logic_error("operation is not running");
    }
    const auto pending = pending_completion_.find(operation);
    if (pending != pending_completion_.end() &&
        (pending->second.outputs != feedback.outputs ||
         pending->second.outcome != feedback.outcome)) {
        throw std::invalid_argument("completion feedback changed while output is waiting");
    }
    if (!upper_.canFinishOperation(operation, feedback.outputs)) {
        throw std::logic_error("operation output positions are occupied");
    }
    upper_.finishOperation(operation, std::move(feedback.outputs),
                           std::move(feedback.outcome));
    pending_completion_.erase(operation);
    remaining_cycles_.erase(operation);
    auto blockages = fixed_blockages_;
    const auto active = upper_.activeBlockages();
    blockages.insert(blockages.end(), active.begin(), active.end());
    router_.setBlockages(blockages);
    router_.setUpcomingBlockages(upper_.upcomingBlockages());
    router_.prepareUpcomingCrossings();
}

bool ExecutionLoop::allDone() const {
    return dag_.allDone() && remaining_cycles_.empty() &&
           pending_completion_.empty() && !router_.hasTasks();
}

int ExecutionLoop::remainingCycles(OperationInstanceId operation) const {
    return remaining_cycles_.at(operation);
}

}  // namespace dmf
