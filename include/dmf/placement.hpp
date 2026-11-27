#pragma once

#include "dmf/dynamic_dag.hpp"

#include <map>
#include <optional>
#include <vector>

namespace dmf {

struct Point {
    int x;
    int y;
};

bool operator==(Point left, Point right);
using DropletPositions = std::map<DropletId, Point>;

struct ModuleProfile {
    int width;
    int height;
    int duration_cycles = 1;
};

struct VirtualModule {
    int id;
    Point top_left;
    int side;

    Point center() const;
};

// Virtual modules are separated by corridors one module wide.
class VirtualChip {
public:
    VirtualChip(int columns, int rows, int module_side = 3);
    VirtualChip(int columns, int rows, int module_side,
                int electrode_width, int electrode_height);

    bool contains(Point point) const;
    int width() const { return width_; }
    int height() const { return height_; }
    int moduleSide() const { return module_side_; }
    std::optional<VirtualModule> nearestFree(
        const std::vector<Point>& input_positions, ModuleProfile profile,
        const std::vector<Point>& occupied = {}) const;
    void reserve(int module_id, OperationInstanceId operation);
    void release(int module_id, OperationInstanceId operation);
    bool isReserved(int module_id) const;

private:
    int columns_;
    int rows_;
    int module_side_;
    int width_;
    int height_;
    std::vector<VirtualModule> modules_;
    std::map<int, OperationInstanceId> reservations_;
};

}  // namespace dmf
