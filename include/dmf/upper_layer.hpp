#pragma once

#include "dmf/routing_task.hpp"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace dmf {

// Coordinates the dynamic DAG, virtual-module placement, and arrival feedback.
class UpperLayer {
public:
    UpperLayer(DynamicDag& dag, VirtualChip chip,
               std::map<OperationType, ModuleProfile> profiles,
               DropletPositions& positions,
               std::map<OperationType, Point> terminal_ports = {},
               std::map<std::string, Point> dispense_ports = {},
               int minimum_separation = 2);

    std::vector<RoutingTask> placeReadyOperations();
    bool reportArrival(OperationInstanceId operation, DropletId droplet,
                       const std::set<int>& held_modules = {});
    std::vector<OperationInstanceId> tryStartPlacedOperations(
        const std::set<int>& held_modules = {});
    void finishOperation(OperationInstanceId operation, std::vector<DropletId> outputs,
                         std::optional<std::string> outcome = std::nullopt);
    bool canFinishOperation(OperationInstanceId operation,
                            const std::vector<DropletId>& outputs) const;

    bool hasPlacement(OperationInstanceId operation) const;
    int durationFor(OperationInstanceId operation) const;
    const VirtualModule& moduleFor(OperationInstanceId operation) const;
    std::vector<VirtualModule> activeBlockages() const;
    std::vector<UpcomingBlockage> upcomingBlockages() const;
    Point positionOf(DropletId droplet) const;
    int minimumSeparation() const { return minimum_separation_; }

private:
    struct Placement {
        std::optional<VirtualModule> module;
        std::vector<DropletId> inputs;
        std::map<DropletId, Point> targets;
        std::set<DropletId> arrived;
        bool running = false;
        std::optional<Point> dispense_port;
    };

    DynamicDag& dag_;
    VirtualChip chip_;
    std::map<OperationType, ModuleProfile> profiles_;
    std::map<OperationType, Point> terminal_ports_;
    std::map<std::string, Point> dispense_ports_;
    int minimum_separation_;
    DropletPositions& positions_;
    std::map<OperationInstanceId, Placement> placements_;

    static std::vector<Point> targetsFor(OperationType type,
                                         const VirtualModule& module,
                                         std::size_t input_count);
    bool tryStart(Placement& placement, OperationInstanceId operation,
                  const std::set<int>& held_modules);
    bool portAvailable(Point port) const;
    std::vector<Point> outputPositions(OperationInstanceId operation,
                                        const Placement& placement) const;
    bool outputSpaceFree(const Placement& placement,
                         const std::vector<Point>& outputs) const;
};

}  // namespace dmf
