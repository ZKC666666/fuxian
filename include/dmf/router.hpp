#pragma once

#include "dmf/routing_task.hpp"

#include <array>
#include <map>
#include <tuple>
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

// One cached BFS field per target; each step still checks the latest hard constraints.
class Router {
public:
    Router(int width, int height, DropletPositions& positions,
           int minimum_separation = 2);

    std::vector<RouteArrival> addTasks(const std::vector<RoutingTask>& tasks);
    void setBlockages(const std::vector<VirtualModule>& modules);
    std::vector<MoveCommand> planStep();
    std::vector<RouteArrival> applyFeedback(const std::vector<MoveFeedback>& feedback);

    bool hasTasks() const { return !tasks_.empty(); }
    int costFieldBuilds() const { return field_builds_; }

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
    std::vector<unsigned char> blocked_;
    std::vector<std::tuple<int, int, int, int>> blockage_signature_;
    std::map<int, std::vector<int>> fields_;
    std::vector<MoveCommand> pending_;
    int field_builds_ = 0;

    bool contains(Point point) const;
    int index(Point point) const;
    Point pointAt(int index) const;
    std::array<Point, 4> neighbors(Point point) const;
    Occupancy occupancy() const;
    bool legal(Point candidate, DropletId droplet, const Occupancy& occupancy) const;
    int futureCost(Point candidate, DropletId droplet,
                   const Occupancy& occupancy, const std::vector<int>& field) const;
    const std::vector<int>& fieldFor(Point target);
};

}  // namespace dmf
