#include "dmf/placement.hpp"

#include <limits>
#include <stdexcept>

namespace dmf {

bool operator==(Point left, Point right) {
    return left.x == right.x && left.y == right.y;
}

Point VirtualModule::center() const {
    return {top_left.x + side / 2, top_left.y + side / 2};
}

VirtualChip::VirtualChip(int columns, int rows, int module_side)
    : columns_(columns), rows_(rows), module_side_(module_side) {
    if (columns <= 0 || rows <= 0 || module_side < 3 || module_side % 2 == 0) {
        throw std::invalid_argument("invalid virtual chip dimensions");
    }
    int id = 1;
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            const Point top_left{module_side * (2 * column + 1),
                                 module_side * (2 * row + 1)};
            modules_.push_back({id++, top_left, module_side});
        }
    }
}

bool VirtualChip::contains(Point point) const {
    const int width = module_side_ * (2 * columns_ + 1);
    const int height = module_side_ * (2 * rows_ + 1);
    return point.x >= 0 && point.x < width && point.y >= 0 && point.y < height;
}

std::optional<VirtualModule> VirtualChip::nearestFree(
    const std::vector<Point>& input_positions, ModuleProfile profile) const {
    if (input_positions.empty() || profile.width <= 0 || profile.height <= 0 ||
        profile.width > module_side_ || profile.height > module_side_) {
        throw std::invalid_argument("invalid module placement request");
    }
    long long sum_x = 0;
    long long sum_y = 0;
    for (const auto point : input_positions) {
        if (!contains(point)) throw std::invalid_argument("input position is outside chip");
        sum_x += point.x;
        sum_y += point.y;
    }

    std::optional<VirtualModule> best;
    long long best_distance = std::numeric_limits<long long>::max();
    const auto count = static_cast<long long>(input_positions.size());
    for (const auto& module : modules_) {
        if (isReserved(module.id)) continue;
        const auto center = module.center();
        const long long dx = count * center.x - sum_x;
        const long long dy = count * center.y - sum_y;
        const long long distance = dx * dx + dy * dy;
        // Row-major module order resolves ties reproducibly.
        if (distance < best_distance) {
            best = module;
            best_distance = distance;
        }
    }
    return best;
}

void VirtualChip::reserve(int module_id, OperationInstanceId operation) {
    if (module_id <= 0 || module_id > static_cast<int>(modules_.size())) {
        throw std::out_of_range("unknown virtual module");
    }
    if (!reservations_.emplace(module_id, operation).second) {
        throw std::logic_error("virtual module is already reserved");
    }
}

void VirtualChip::release(int module_id, OperationInstanceId operation) {
    const auto found = reservations_.find(module_id);
    if (found == reservations_.end() || found->second != operation) {
        throw std::logic_error("virtual module reservation does not match operation");
    }
    reservations_.erase(found);
}

bool VirtualChip::isReserved(int module_id) const {
    return reservations_.count(module_id) != 0;
}

}  // namespace dmf
