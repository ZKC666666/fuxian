#pragma once

#include "dmf/routing_task.hpp"

#include <istream>
#include <vector>

namespace dmf::demo {

struct RouteCase {
    int width = 0;
    int height = 0;
    int separation = 2;
    int max_steps = 100;
    bool trace = true;
    DropletPositions positions;
    std::vector<RoutingTask> tasks;
};

RouteCase parseRouteCase(std::istream& input);

}  // namespace dmf::demo
