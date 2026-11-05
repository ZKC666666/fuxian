#pragma once

#include "dmf/placement.hpp"

namespace dmf {

// The shared message between placement and any lower-level router.
struct RoutingTask {
    OperationInstanceId operation;
    DropletId droplet;
    Point target;
    int module_id;
};

}  // namespace dmf
