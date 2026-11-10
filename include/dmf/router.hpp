#pragma once

#include "dmf/cost_field_updater.hpp"
#include "dmf/routing_task.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace dmf {

struct RouteArrival {
    OperationInstanceId operation;
    DropletId droplet;
};

struct MoveCommand {
    DropletId droplet;
    Point from;
    Point to;
};

struct MoveFeedback {
    DropletId droplet;
    Point observed;
};

// Cached BFS fields account for the target and its allowed upcoming module.
class Router {
public:
    Router(int width, int height, DropletPositions& positions,
           int minimum_separation = 2);

    std::vector<RouteArrival> addTasks(const std::vector<RoutingTask>& tasks);
    void setBlockages(const std::vector<VirtualModule>& modules);
    void setUpcomingBlockages(const std::vector<UpcomingBlockage>& modules);
    std::vector<MoveCommand> planStep();
    std::vector<RouteArrival> applyFeedback(const std::vector<MoveFeedback>& feedback);
    void waitForCostFieldUpdates();  // Explicit barrier for reproducible experiments.

    bool hasTasks() const;
    int costFieldBuilds() const { return field_builds_; }
    int asyncFieldSwaps() const { return async_field_swaps_; }
    int staleFieldDecisions() const { return stale_field_decisions_; }
    const std::map<DropletId, Point>& evacuationTargets() const {
        return evacuation_targets_;
    }
    const std::vector<DropletId>& evacuatedLastStep() const {
        return evacuated_last_step_;
    }
    const std::vector<DropletId>& waitingForEvacuation() const {
        return waiting_for_evacuation_;
    }

private:
    struct Occupancy {
        std::vector<DropletId> current;
        std::vector<DropletId> previous;
        std::vector<DropletId> planned;
    };

    int width_;
    int height_;
    int minimum_separation_;
    DropletPositions& positions_;
    DropletPositions previous_positions_;
    std::map<DropletId, RoutingTask> tasks_;
    std::vector<int> upcoming_region_;
    std::map<DropletId, int> upcoming_input_owners_;
    std::map<DropletId, Point> evacuation_targets_;
    std::vector<DropletId> evacuated_last_step_;
    std::vector<DropletId> waiting_for_evacuation_;
    std::vector<unsigned char> blocked_;
    std::vector<std::tuple<int, int, int, int>> blockage_signature_;
    std::vector<std::tuple<int, int, int, int, std::vector<DropletId>>>
        upcoming_signature_;
    std::map<std::pair<int, int>, std::vector<int>> fields_;
    std::map<CostFieldKey, std::uint64_t> field_versions_;
    std::shared_ptr<const CostFieldSnapshot> field_snapshot_;
    CostFieldUpdater field_updater_;
    std::vector<MoveCommand> pending_;
    int field_builds_ = 0;
    int async_field_swaps_ = 0;
    int stale_field_decisions_ = 0;

    bool contains(Point point) const;
    int index(Point point) const;
    std::array<Point, 4> neighbors(Point point) const;
    Occupancy occupancy() const;
    bool legal(Point candidate, DropletId droplet, const Occupancy& occupancy) const;
    bool upcomingBlocks(Point point, int allowed_module) const;
    int upcomingRegion(Point point) const;
    bool needsEvacuation(DropletId droplet, Point position) const;
    std::optional<Point> nearestExit(DropletId droplet, const Occupancy& occupied) const;
    void refreshEvacuations(const Occupancy& occupied);
    void discardUnusedFields(Point target);
    void environmentChanged();
    void installUpdatedField(CostFieldKey key, std::vector<int> field);
    int futureCost(Point candidate, DropletId droplet,
                   const Occupancy& occupancy, const std::vector<int>& field) const;
    const std::vector<int>& fieldFor(Point target, int allowed_module);
};

}  // namespace dmf
