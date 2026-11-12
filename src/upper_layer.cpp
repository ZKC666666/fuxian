#include "dmf/upper_layer.hpp"

#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <utility>

namespace dmf {

UpperLayer::UpperLayer(DynamicDag& dag, VirtualChip chip,
                       std::map<OperationType, ModuleProfile> profiles,
                       DropletPositions& positions,
                       std::map<OperationType, Point> terminal_ports,
                       std::map<std::string, Point> dispense_ports,
                       int minimum_separation)
    : dag_(dag), chip_(std::move(chip)), profiles_(std::move(profiles)),
      terminal_ports_(std::move(terminal_ports)),
      dispense_ports_(std::move(dispense_ports)),
      minimum_separation_(minimum_separation), positions_(positions) {
    if (minimum_separation_ < 1) {
        throw std::invalid_argument("invalid fluidic separation");
    }
    for (const auto& [type, profile] : profiles_) {
        const bool module_profile =
            (type == OperationType::Mix || type == OperationType::Detect ||
             type == OperationType::Split) &&
            profile.width > 0 && profile.height > 0 &&
            profile.width <= chip_.moduleSide() &&
            profile.height <= chip_.moduleSide() &&
            (type != OperationType::Mix || profile.width >= 3) &&
            (type != OperationType::Split ||
             (profile.width == 3 && profile.height == 1));
        const bool dispense_profile = type == OperationType::Dispense &&
                                      profile.width == 1 && profile.height == 1;
        if (profile.duration_cycles <= 0 ||
            (!module_profile && !dispense_profile) ||
            (type == OperationType::Split && minimum_separation_ > 2)) {
            throw std::invalid_argument("unsupported module profile");
        }
    }
    for (const auto& [droplet, position] : positions_) {
        if (droplet <= 0 || !chip_.contains(position)) {
            throw std::invalid_argument("invalid initial droplet position");
        }
    }
    for (const auto& [type, port] : terminal_ports_) {
        if ((type != OperationType::Output && type != OperationType::Waste) ||
            !chip_.contains(port) ||
            (port.x != 0 && port.x != chip_.width() - 1 &&
             port.y != 0 && port.y != chip_.height() - 1)) {
            throw std::invalid_argument("invalid terminal port");
        }
    }
    if (terminal_ports_.count(OperationType::Output) != 0 &&
        terminal_ports_.count(OperationType::Waste) != 0 &&
        terminal_ports_.at(OperationType::Output) == terminal_ports_.at(OperationType::Waste)) {
        throw std::invalid_argument("output and waste ports must differ");
    }
    for (const auto& [name, port] : dispense_ports_) {
        if (name.empty() || !chip_.contains(port) ||
            (port.x != 0 && port.x != chip_.width() - 1 &&
             port.y != 0 && port.y != chip_.height() - 1)) {
            throw std::invalid_argument("invalid dispense port");
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
    if ((type == OperationType::Detect || type == OperationType::Split) &&
        input_count == 1) return {center};
    throw std::invalid_argument("operation has unsupported input count");
}

std::vector<RoutingTask> UpperLayer::placeReadyOperations() {
    std::vector<RoutingTask> tasks;
    for (const auto operation : dag_.readyOperations()) {
        const auto type = dag_.operationType(operation);
        const auto inputs = dag_.inputsIfReady(operation);
        if (!inputs) continue;  // An earlier placement may have reserved a shared droplet.

        if (type == OperationType::Dispense) {
            const auto& name = dag_.operation(operation).spec_id;
            const auto port = dispense_ports_.find(name);
            if (port == dispense_ports_.end() ||
                profiles_.count(OperationType::Dispense) == 0) {
                throw std::logic_error("dispense operation has no port or profile");
            }
            if (!inputs->empty()) throw std::invalid_argument("dispense has input droplets");
            if (!portAvailable(port->second)) continue;
            dag_.reserveOperation(operation);
            placements_.emplace(operation,
                                Placement{std::nullopt, {}, {}, {}, false,
                                          port->second});
            continue;
        }

        if (type == OperationType::Output || type == OperationType::Waste) {
            const auto port = terminal_ports_.find(type);
            if (port == terminal_ports_.end()) {
                throw std::logic_error("terminal operation has no configured port");
            }
            if (inputs->size() != 1) {
                throw std::invalid_argument("terminal operation requires one input droplet");
            }
            dag_.reserveOperation(operation);
            Placement placement{std::nullopt, *inputs, {}, {}, false};
            placement.targets.emplace(inputs->front(), port->second);
            placements_.emplace(operation, std::move(placement));
            tasks.push_back({operation, inputs->front(), port->second, 0});
            continue;
        }

        const auto profile = profiles_.find(type);
        if (profile == profiles_.end()) continue;

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

bool UpperLayer::reportArrival(OperationInstanceId operation, DropletId droplet,
                               const std::set<int>& held_modules) {
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
    return tryStart(placement, operation, held_modules);
}

bool UpperLayer::tryStart(Placement& placement, OperationInstanceId operation,
                          const std::set<int>& held_modules) {
    if (placement.running || placement.arrived.size() != placement.inputs.size()) return false;
    if (placement.dispense_port &&
        !outputSpaceFree(placement, {*placement.dispense_port})) return false;
    if (placement.module) {
        const auto& module = *placement.module;
        if (held_modules.count(module.id) != 0) return false;
        for (const auto& [droplet, position] : positions_) {
            if (placement.targets.count(droplet) != 0) continue;
            if (position.x >= module.top_left.x &&
                position.x < module.top_left.x + module.side &&
                position.y >= module.top_left.y &&
                position.y < module.top_left.y + module.side) {
                return false;
            }
        }
    }
    dag_.startOperation(operation);
    placement.running = true;
    return true;
}

bool UpperLayer::portAvailable(Point port) const {
    for (const auto& [droplet, position] : positions_) {
        (void)droplet;
        if (std::max(std::abs(port.x - position.x),
                     std::abs(port.y - position.y)) < minimum_separation_) return false;
    }
    for (const auto& [operation, placement] : placements_) {
        (void)operation;
        if (!placement.dispense_port) continue;
        const auto other = *placement.dispense_port;
        if (std::max(std::abs(port.x - other.x),
                     std::abs(port.y - other.y)) < minimum_separation_) return false;
    }
    return true;
}

std::vector<Point> UpperLayer::outputPositions(
    OperationInstanceId operation, const Placement& placement) const {
    switch (dag_.operationType(operation)) {
        case OperationType::Dispense: return {*placement.dispense_port};
        case OperationType::Mix:
        case OperationType::Detect: return {placement.module->center()};
        case OperationType::Split: {
            const auto center = placement.module->center();
            return {{center.x - 1, center.y}, {center.x + 1, center.y}};
        }
        case OperationType::Output:
        case OperationType::Waste: return {};
    }
    throw std::logic_error("unknown operation type");
}

bool UpperLayer::outputSpaceFree(const Placement& placement,
                                  const std::vector<Point>& outputs) const {
    for (std::size_t index = 0; index < outputs.size(); ++index) {
        if (!chip_.contains(outputs[index])) return false;
        for (std::size_t other = 0; other < index; ++other) {
            const auto distance = std::max(std::abs(outputs[index].x - outputs[other].x),
                                           std::abs(outputs[index].y - outputs[other].y));
            if (distance < minimum_separation_) return false;
        }
        for (const auto& [droplet, position] : positions_) {
            if (std::find(placement.inputs.begin(), placement.inputs.end(), droplet) !=
                placement.inputs.end()) continue;
            const auto distance = std::max(std::abs(outputs[index].x - position.x),
                                           std::abs(outputs[index].y - position.y));
            if (distance < minimum_separation_) return false;
        }
    }
    return true;
}

bool UpperLayer::canFinishOperation(
    OperationInstanceId operation, const std::vector<DropletId>& outputs) const {
    const auto& placement = placements_.at(operation);
    if (!placement.running) throw std::logic_error("operation has not started");
    const auto locations = outputPositions(operation, placement);
    if (outputs.size() != locations.size()) {
        throw std::invalid_argument("wrong number of physical operation outputs");
    }
    for (const auto droplet : outputs) {
        if (positions_.count(droplet) != 0) {
            throw std::invalid_argument("operation output id already occupies the chip");
        }
    }
    return outputSpaceFree(placement, locations);
}

std::vector<OperationInstanceId> UpperLayer::tryStartPlacedOperations(
    const std::set<int>& held_modules) {
    std::vector<OperationInstanceId> started;
    for (auto& [operation, placement] : placements_) {
        if (tryStart(placement, operation, held_modules)) started.push_back(operation);
    }
    return started;
}

void UpperLayer::finishOperation(OperationInstanceId operation,
                                  std::vector<DropletId> outputs,
                                  std::optional<std::string> outcome) {
    auto& placement = placements_.at(operation);
    if (!canFinishOperation(operation, outputs)) {
        throw std::logic_error("operation output positions are occupied");
    }
    const auto locations = outputPositions(operation, placement);
    dag_.finishOperation(operation, outputs, std::move(outcome));
    for (const auto droplet : placement.inputs) positions_.erase(droplet);
    for (std::size_t index = 0; index < outputs.size(); ++index) {
        positions_[outputs[index]] = locations[index];
    }
    if (placement.module) {
        chip_.release(placement.module->id, operation);
    }
    placements_.erase(operation);
}

bool UpperLayer::hasPlacement(OperationInstanceId operation) const {
    return placements_.count(operation) != 0;
}

int UpperLayer::durationFor(OperationInstanceId operation) const {
    const auto type = dag_.operationType(operation);
    if (type == OperationType::Output || type == OperationType::Waste) return 1;
    return profiles_.at(type).duration_cycles;
}

const VirtualModule& UpperLayer::moduleFor(OperationInstanceId operation) const {
    const auto& module = placements_.at(operation).module;
    if (!module) throw std::logic_error("terminal operation has no module");
    return *module;
}

std::vector<VirtualModule> UpperLayer::activeBlockages() const {
    std::vector<VirtualModule> blockages;
    for (const auto& [operation, placement] : placements_) {
        (void)operation;
        if (placement.running && placement.module) blockages.push_back(*placement.module);
        if (placement.running && placement.dispense_port) {
            blockages.push_back({-operation, *placement.dispense_port, 1});
        }
    }
    return blockages;
}

std::vector<UpcomingBlockage> UpperLayer::upcomingBlockages() const {
    std::vector<UpcomingBlockage> upcoming;
    for (const auto& [operation, placement] : placements_) {
        (void)operation;
        if (!placement.running && placement.module) {
            upcoming.push_back({*placement.module, placement.inputs});
        }
    }
    return upcoming;
}

Point UpperLayer::positionOf(DropletId droplet) const {
    return positions_.at(droplet);
}

}  // namespace dmf
