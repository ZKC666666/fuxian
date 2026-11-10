#pragma once

#include "dmf/routing_task.hpp"

#include <map>
#include <optional>
#include <set>
#include <vector>

namespace dmf {

// Coordinates the dynamic DAG, virtual-module placement, and arrival feedback.
class UpperLayer {
public:
    UpperLayer(DynamicDag& dag, VirtualChip chip,
               std::map<OperationType, ModuleProfile> profiles,
               DropletPositions& positions,
               std::map<OperationType, Point> terminal_ports = {});

    std::vector<RoutingTask> placeReadyOperations();
    bool reportArrival(OperationInstanceId operation, DropletId droplet);
    std::vector<OperationInstanceId> tryStartPlacedOperations();
    void finishOperation(OperationInstanceId operation, std::vector<DropletId> outputs,
                         std::optional<std::string> outcome = std::nullopt);

    bool hasPlacement(OperationInstanceId operation) const;
    int durationFor(OperationInstanceId operation) const;
    const VirtualModule& moduleFor(OperationInstanceId operation) const;
    std::vector<VirtualModule> activeBlockages() const;
    std::vector<UpcomingBlockage> upcomingBlockages() const;
    Point positionOf(DropletId droplet) const;

private:
    struct Placement {
        std::optional<VirtualModule> module;
        std::vector<DropletId> inputs;
        std::map<DropletId, Point> targets;
        std::set<DropletId> arrived;
        bool running = false;
    };

    DynamicDag& dag_;
    VirtualChip chip_;
    std::map<OperationType, ModuleProfile> profiles_;
    std::map<OperationType, Point> terminal_ports_;
    DropletPositions& positions_;
    std::map<OperationInstanceId, Placement> placements_;

    static std::vector<Point> targetsFor(OperationType type,
                                         const VirtualModule& module,
                                         std::size_t input_count);
    bool tryStart(Placement& placement, OperationInstanceId operation);
};

}  // namespace dmf
