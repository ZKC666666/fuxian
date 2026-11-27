#pragma once

#include "dmf/router.hpp"
#include "dmf/upper_layer.hpp"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace dmf {

struct CompletionFeedback {
    std::vector<DropletId> outputs;
    std::optional<std::string> outcome;
};

enum class BranchActivationMode { Immediate, AfterBatch, StaticBatch };

struct StepReport {
    int step = 0;
    std::vector<BlockInstanceId> activated_blocks;
    std::vector<RoutingTask> routing_tasks;
    std::vector<RouteArrival> route_arrivals;
    std::vector<MoveCommand> moves;
    std::vector<MoveFeedback> observed;
    std::map<DropletId, Point> evacuation_targets;
    std::map<DropletId, int> crossing_modules;
    std::vector<DropletId> waiting_for_crossing;
    std::vector<int> held_modules;
    std::vector<DropletId> evacuated;
    std::vector<DropletId> waiting_for_evacuation;
    std::vector<OperationInstanceId> started;
    std::vector<OperationInstanceId> completed;
    std::vector<OperationInstanceId> waiting_for_output_space;
    std::size_t active_blockages = 0;
    int new_cost_fields = 0;
    int async_field_swaps = 0;
    int stale_field_decisions = 0;
    long long routing_microseconds = 0;
    long long batch_planning_microseconds = 0;
    long long replay_microseconds = 0;
    int batch_plans = 0;
    std::size_t planned_route_steps = 0;
    int replayed_route_steps = 0;
    long long connectivity_check_microseconds = 0;
    long long blockage_update_microseconds = 0;
};

// Simulates one control step at a time. Callers may supply observed movement;
// by default every commanded move succeeds.
class ExecutionLoop {
public:
    using CompletionFeedbackProvider =
        std::function<CompletionFeedback(OperationInstanceId)>;
    using MovementFeedbackProvider = std::function<Point(const MoveCommand&, int step)>;

    ExecutionLoop(DynamicDag& dag, UpperLayer& upper, Router& router,
                  CompletionFeedbackProvider completion_feedback_provider,
                  MovementFeedbackProvider movement_provider = {},
                  std::vector<VirtualModule> fixed_blockages = {},
                  BranchActivationMode branch_mode = BranchActivationMode::Immediate);

    StepReport step(RouteDecisionTrace* trace = nullptr);
    void completeEarly(OperationInstanceId operation, CompletionFeedback feedback);
    bool allDone() const;
    int remainingCycles(OperationInstanceId operation) const;

private:
    DynamicDag& dag_;
    UpperLayer& upper_;
    Router& router_;
    CompletionFeedbackProvider completion_feedback_provider_;
    MovementFeedbackProvider movement_provider_;
    std::vector<VirtualModule> fixed_blockages_;
    BranchActivationMode branch_mode_;
    std::map<OperationInstanceId, int> remaining_cycles_;
    std::map<OperationInstanceId, CompletionFeedback> pending_completion_;
    int step_count_ = 0;
    std::optional<TerminalBatchPlan> terminal_plan_;
    std::size_t next_planned_step_ = 0;

    void recordStart(OperationInstanceId operation, StepReport& report);
    void acceptArrivals(const std::vector<RouteArrival>& arrivals, StepReport& report);
    void acceptStarts(const std::vector<OperationInstanceId>& started, StepReport& report);
    void updateBlockages(StepReport& report);
    void prepareCrossings(StepReport& report);
    void prepareTerminalBatch(StepReport& report);
    bool canFinishOperation(OperationInstanceId operation,
                            const std::vector<DropletId>& outputs) const;
};

}  // namespace dmf
