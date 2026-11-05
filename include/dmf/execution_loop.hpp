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

struct StepReport {
    int step = 0;
    std::vector<MoveCommand> moves;
    std::vector<OperationInstanceId> started;
    std::vector<OperationInstanceId> completed;
    std::size_t active_blockages = 0;
    int new_cost_fields = 0;
    long long routing_microseconds = 0;
    long long blockage_update_microseconds = 0;
};

// Simulates one control step at a time. Movement feedback equals the command;
// completion feedback is supplied by the caller when an operation finishes.
class ExecutionLoop {
public:
    using FeedbackProvider = std::function<CompletionFeedback(OperationInstanceId)>;

    ExecutionLoop(DynamicDag& dag, UpperLayer& upper, Router& router,
                  FeedbackProvider feedback_provider);

    StepReport step();
    void completeEarly(OperationInstanceId operation, CompletionFeedback feedback);
    bool allDone() const;
    int remainingCycles(OperationInstanceId operation) const;

private:
    DynamicDag& dag_;
    UpperLayer& upper_;
    Router& router_;
    FeedbackProvider feedback_provider_;
    std::map<OperationInstanceId, int> remaining_cycles_;
    int step_count_ = 0;

    void recordStart(OperationInstanceId operation, StepReport& report);
    void acceptArrivals(const std::vector<RouteArrival>& arrivals, StepReport& report);
    void acceptStarts(const std::vector<OperationInstanceId>& started, StepReport& report);
    void updateBlockages(StepReport& report);
};

}  // namespace dmf
