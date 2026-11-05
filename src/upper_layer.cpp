#include "dmf/upper_layer.hpp"

#include <stdexcept>
#include <utility>

namespace dmf {

UpperLayer::UpperLayer(DynamicDag& dag, VirtualChip chip,
                       std::map<OperationType, ModuleProfile> profiles,
                       DropletPositions& positions)
    : dag_(dag), chip_(std::move(chip)), profiles_(std::move(profiles)),
      positions_(positions) {
    for (const auto& [type, profile] : profiles_) {
        if ((type != OperationType::Mix && type != OperationType::Detect) ||
            profile.width <= 0 || profile.height <= 0 ||
            profile.duration_cycles <= 0 ||
            profile.width > chip_.moduleSide() || profile.height > chip_.moduleSide() ||
            (type == OperationType::Mix && profile.width < 3)) {
            throw std::invalid_argument("unsupported module profile");
        }
    }
    for (const auto& [droplet, position] : positions_) {
        if (droplet <= 0 || !chip_.contains(position)) {
            throw std::invalid_argument("invalid initial droplet position");
        }
    }
}

std::vector<Point> UpperLayer::targetsFor(OperationType type,
                                           const VirtualModule& module,
                                           std::size_t input_count) {
    const auto center = module.center();
    if (type == OperationType::Mix && input_count == 2) {
        return {{center.x - 1, center.y}, {center.x + 1, center.y}};
    }
    if (type == OperationType::Detect && input_count == 1) return {center};
    throw std::invalid_argument("operation has unsupported input count");
}

std::vector<RoutingTask> UpperLayer::placeReadyOperations() {
    std::vector<RoutingTask> tasks;
    for (const auto operation : dag_.readyOperations()) {
        const auto profile = profiles_.find(dag_.operationType(operation));
        if (profile == profiles_.end()) continue;
        const auto inputs = dag_.inputsIfReady(operation);
        if (!inputs) continue;  // An earlier placement may have reserved a shared droplet.

        std::vector<Point> positions;
        for (const auto droplet : *inputs) positions.push_back(positions_.at(droplet));
        const auto module = chip_.nearestFree(positions, profile->second);
        if (!module) continue;
        const auto targets = targetsFor(profile->first, *module, inputs->size());

        dag_.reserveOperation(operation);
        chip_.reserve(module->id, operation);
        Placement placement{*module, *inputs, {}, {}, false};
        for (std::size_t index = 0; index < inputs->size(); ++index) {
            placement.targets.emplace(inputs->at(index), targets.at(index));
            tasks.push_back({operation, inputs->at(index), targets.at(index), module->id});
        }
        placements_.emplace(operation, std::move(placement));
    }
    return tasks;
}

bool UpperLayer::reportArrival(OperationInstanceId operation, DropletId droplet) {
    auto& placement = placements_.at(operation);
    if (placement.running) throw std::logic_error("operation is already running");
    const auto target = placement.targets.find(droplet);
    if (target == placement.targets.end()) {
        throw std::invalid_argument("droplet is not routed to this operation");
    }
    if (!(positions_.at(droplet) == target->second)) {
        throw std::logic_error("droplet has not reached its routing target");
    }
    placement.arrived.insert(droplet);
    return tryStart(placement, operation);
}

bool UpperLayer::tryStart(Placement& placement, OperationInstanceId operation) {
    if (placement.running || placement.arrived.size() != placement.inputs.size()) return false;
    const auto& module = placement.module;
    for (const auto& [droplet, position] : positions_) {
        if (placement.targets.count(droplet) != 0) continue;
        if (position.x >= module.top_left.x &&
            position.x < module.top_left.x + module.side &&
            position.y >= module.top_left.y &&
            position.y < module.top_left.y + module.side) {
            return false;
        }
    }
    dag_.startOperation(operation);
    placement.running = true;
    return true;
}

std::vector<OperationInstanceId> UpperLayer::tryStartPlacedOperations() {
    std::vector<OperationInstanceId> started;
    for (auto& [operation, placement] : placements_) {
        if (tryStart(placement, operation)) started.push_back(operation);
    }
    return started;
}

void UpperLayer::finishOperation(OperationInstanceId operation,
                                  std::vector<DropletId> outputs,
                                  std::optional<std::string> outcome) {
    auto& placement = placements_.at(operation);
    if (!placement.running) throw std::logic_error("operation has not started");
    dag_.finishOperation(operation, outputs, std::move(outcome));
    for (const auto droplet : placement.inputs) positions_.erase(droplet);
    for (const auto droplet : outputs) positions_[droplet] = placement.module.center();
    chip_.release(placement.module.id, operation);
    placements_.erase(operation);
}

bool UpperLayer::hasPlacement(OperationInstanceId operation) const {
    return placements_.count(operation) != 0;
}

int UpperLayer::durationFor(OperationInstanceId operation) const {
    return profiles_.at(dag_.operationType(operation)).duration_cycles;
}

const VirtualModule& UpperLayer::moduleFor(OperationInstanceId operation) const {
    return placements_.at(operation).module;
}

std::vector<VirtualModule> UpperLayer::activeBlockages() const {
    std::vector<VirtualModule> blockages;
    for (const auto& [operation, placement] : placements_) {
        (void)operation;
        if (placement.running) blockages.push_back(placement.module);
    }
    return blockages;
}

Point UpperLayer::positionOf(DropletId droplet) const {
    return positions_.at(droplet);
}

}  // namespace dmf
