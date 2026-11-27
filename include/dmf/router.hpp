#pragma once

#include "dmf/cost_field_updater.hpp"
#include "dmf/routing_task.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string_view>
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

using TerminalBatchPlan = std::vector<std::vector<MoveCommand>>;

enum class CostFieldMode { Asynchronous, WaitForLatest };

struct RouteCandidateTrace {
    Point point;
    std::string_view rejection;
    int cost = 0;
    int future_cost = 0;
    bool backtracks = false;
};

struct RouteDecisionTrace {
    DropletId droplet;
    Point from{};
    Point target{};
    Point chosen{};
    std::string_view action{};
    std::uint64_t layout_version = 0;
    std::optional<std::uint64_t> field_version{};
    std::vector<RouteCandidateTrace> candidates{};
};

// Cached BFS fields account for the target and its allowed upcoming module.
class Router {
public:
    Router(int width, int height, DropletPositions& positions,
           int minimum_separation = 2,
           CostFieldMode field_mode = CostFieldMode::Asynchronous);

    std::vector<RouteArrival> addTasks(const std::vector<RoutingTask>& tasks);
    void setBlockages(const std::vector<VirtualModule>& modules);
    void setUpcomingBlockages(const std::vector<UpcomingBlockage>& modules);
    std::vector<MoveCommand> planStep(RouteDecisionTrace* trace = nullptr);
    TerminalBatchPlan planTerminalBatch(const std::vector<RoutingTask>& tasks) const;
    void replayTerminalStep(const std::vector<MoveCommand>& moves);
    std::vector<RouteArrival> applyFeedback(const std::vector<MoveFeedback>& feedback);
    void recordIdleStep();
    bool hasPreviousClearance(Point point) const;
    void prepareUpcomingCrossings();
    void waitForCostFieldUpdates();  // Explicit barrier for reproducible experiments.

    bool hasTasks() const;
    int minimumSeparation() const { return minimum_separation_; }
    int costFieldBuilds() const { return field_builds_; }
    int asyncFieldSwaps() const { return async_field_swaps_; }
    int staleFieldDecisions() const { return stale_field_decisions_; }
    int detourSearches() const { return detour_searches_; }
    int synchronousFieldRefreshes() const { return synchronous_field_refreshes_; }
    const std::set<int>& activationHolds() const { return activation_holds_; }
    const std::map<DropletId, int>& crossingModules() const {
        return crossing_modules_;
    }
    const std::vector<DropletId>& waitingForCrossing() const {
        return waiting_for_crossing_;
    }
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

    struct DetourState {
        // Two observations of a cycle of up to four moves.
        std::array<Point, 8> history{};
        std::size_t length = 0;
        std::vector<Point> remaining;

        bool observeCycle(Point current);
    };

    int width_;
    int height_;
    int minimum_separation_;
    CostFieldMode field_mode_;
    DropletPositions& positions_;
    DropletPositions previous_positions_;
    std::map<DropletId, RoutingTask> tasks_;
    std::map<DropletId, DetourState> detours_;
    std::vector<int> upcoming_region_;
    std::map<DropletId, int> upcoming_input_owners_;
    std::vector<UpcomingBlockage> upcoming_blockages_;
    std::map<DropletId, int> crossing_modules_;
    std::map<DropletId, std::pair<int, std::deque<Point>>> crossing_paths_;
    std::vector<DropletId> waiting_for_crossing_;
    std::set<int> activation_holds_;
    std::vector<int> open_components_;
    std::map<int, std::vector<int>> future_components_;
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
    int detour_searches_ = 0;
    int synchronous_field_refreshes_ = 0;

    bool contains(Point point) const;
    int index(Point point) const;
    std::array<Point, 4> neighbors(Point point) const;
    Occupancy occupancy() const;
    const char* positionRejection(Point candidate, DropletId droplet,
                                  const Occupancy& occupied) const;
    bool legal(Point candidate, DropletId droplet, const Occupancy& occupied) const;
    bool upcomingBlocks(Point point, int allowed_module) const;
    int upcomingRegion(Point point) const;
    bool needsEvacuation(DropletId droplet, Point position) const;
    std::optional<Point> nearestExit(DropletId droplet, const Occupancy& occupied) const;
    void refreshEvacuations(const Occupancy& occupied);
    void discardUnusedFields(Point target);
    void environmentChanged();
    std::vector<int> components(const std::vector<unsigned char>& blocked) const;
    const std::vector<int>& openComponents();
    const std::vector<int>& futureComponents(const VirtualModule& module);
    std::optional<Point> nextCrossingMove(DropletId droplet, int module_id,
                                           const Occupancy& occupied);
    std::optional<Point> nextDetourMove(DropletId droplet, Point target,
                                         int allowed_region, const Occupancy& occupied);
    void installUpdatedField(CostFieldKey key, std::vector<int> field);
    int futureCost(Point candidate, DropletId droplet,
                   int allowed_region, const Occupancy& occupancy,
                   const std::vector<int>& field) const;
    const std::vector<int>& fieldFor(Point target, int allowed_module);
    void refreshUnusableField(CostFieldKey key, const std::array<Point, 4>& candidates,
                              DropletId droplet, int allowed_region,
                              const Occupancy& occupied);
};

}  // namespace dmf
