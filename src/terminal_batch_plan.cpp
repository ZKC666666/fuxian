#include "dmf/router.hpp"

#include <cstdlib>
#include <set>
#include <stdexcept>
#include <utility>

namespace dmf {

TerminalBatchPlan Router::planTerminalBatch(const std::vector<RoutingTask>& tasks) const {
    if (!tasks_.empty() || !pending_.empty() || !upcoming_blockages_.empty() ||
        tasks.size() != positions_.size()) {
        throw std::logic_error("terminal batch requires an idle layout with one task per droplet");
    }
    for (const auto& task : tasks) {
        if (task.module_id != 0) throw std::logic_error("terminal batch has a module task");
    }

    auto positions = positions_;
    Router preview(width_, height_, positions, minimum_separation_, CostFieldMode::WaitForLatest);
    preview.previous_positions_ = previous_positions_;
    std::vector<VirtualModule> fixed_blockages;
    for (const auto& [id, x, y, side] : blockage_signature_) {
        fixed_blockages.push_back({id, {x, y}, side});
    }
    preview.setBlockages(fixed_blockages);

    TerminalBatchPlan plan;
    const auto immediate_arrivals = preview.addTasks(tasks);
    std::vector<RouteArrival> remove_at_end;
    constexpr std::size_t max_route_steps = 1000;
    while (preview.hasTasks() && plan.size() < max_route_steps) {
        plan.push_back(preview.planStep());
        std::vector<MoveFeedback> feedback;
        for (const auto& move : plan.back()) feedback.push_back({move.droplet, move.to});
        auto arrived = preview.applyFeedback(feedback);

        // A terminal operation consumes its droplet one control step after arrival.
        for (const auto& arrival : remove_at_end) positions.erase(arrival.droplet);
        if (plan.size() == 1) {
            arrived.insert(arrived.end(), immediate_arrivals.begin(), immediate_arrivals.end());
        }
        remove_at_end = std::move(arrived);
    }
    if (preview.hasTasks()) {
        throw std::runtime_error("terminal batch planning exceeded 1000 routing steps");
    }
    return plan;
}

void Router::replayTerminalStep(const std::vector<MoveCommand>& moves) {
    if (!pending_.empty() || !upcoming_blockages_.empty() || moves.size() != tasks_.size()) {
        throw std::logic_error("terminal plan does not cover the current tasks");
    }
    auto occupied = occupancy();
    std::set<DropletId> seen;
    for (const auto& move : moves) {
        const auto task = tasks_.find(move.droplet);
        if (task == tasks_.end() || task->second.module_id != 0 || !seen.insert(move.droplet).second ||
            !(positions_.at(move.droplet) == move.from) ||
            !legal(move.to, move.droplet, occupied) ||
            std::abs(move.to.x - move.from.x) + std::abs(move.to.y - move.from.y) > 1) {
            throw std::logic_error("terminal plan does not match the current safe move set");
        }
        occupied.planned[index(move.to)] = move.droplet;
    }
    pending_ = moves;
}

}  // namespace dmf
