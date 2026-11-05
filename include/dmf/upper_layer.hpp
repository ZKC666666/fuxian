#pragma once

#include "dmf/placement.hpp"

#include <map>
#include <optional>
#include <set>
#include <vector>

namespace dmf {

struct RoutingTask {
    OperationInstanceId operation;
    DropletId droplet;
    Point target;
    int module_id;
};

// Coordinates the dynamic DAG, virtual-module placement, and arrival feedback.
class UpperLayer {
public:
    UpperLayer(DynamicDag& dag, VirtualChip chip,
               std::map<OperationType, ModuleProfile> profiles,
               std::map<DropletId, Point> initial_positions);

    std::vector<RoutingTask> placeReadyOperations();
    bool reportArrival(OperationInstanceId operation, DropletId droplet);
    void finishOperation(OperationInstanceId operation, std::vector<DropletId> outputs,
                         std::optional<std::string> outcome = std::nullopt);

    bool hasPlacement(OperationInstanceId operation) const;
    const VirtualModule& moduleFor(OperationInstanceId operation) const;
    std::vector<VirtualModule> activeBlockages() const;
    Point positionOf(DropletId droplet) const;

private:
    struct Placement {
        VirtualModule module;
        std::vector<DropletId> inputs;
        std::map<DropletId, Point> targets;
        std::set<DropletId> arrived;
        bool running = false;
    };

    DynamicDag& dag_;
    VirtualChip chip_;
    std::map<OperationType, ModuleProfile> profiles_;
    std::map<DropletId, Point> positions_;
    std::map<OperationInstanceId, Placement> placements_;

    static std::vector<Point> targetsFor(OperationType type,
                                         const VirtualModule& module,
                                         std::size_t input_count);
};

}  // namespace dmf
