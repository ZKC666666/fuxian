#include "dmf/router.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename ExpectedError, typename Action>
void rejects(Action action, const std::string& message) {
    try {
        action();
    } catch (const ExpectedError&) {
        return;
    }
    throw std::runtime_error(message);
}

std::vector<dmf::RouteArrival> advanceAsCommanded(dmf::Router& router) {
    std::vector<dmf::MoveFeedback> feedback;
    for (const auto& command : router.planStep()) {
        feedback.push_back({command.droplet, command.to});
    }
    return router.applyFeedback(feedback);
}

int distance(dmf::Point left, dmf::Point right) {
    return std::max(std::abs(left.x - right.x), std::abs(left.y - right.y));
}

void cachedFieldAndMovementFailure() {
    dmf::DropletPositions positions{{1, {0, 4}}};
    dmf::Router router(9, 9, positions);
    check(router.addTasks({{1, 1, {6, 4}, 1}}).empty(),
          "routing task arrived before movement");
    const auto first = router.planStep();
    check(first.size() == 1 && first[0].to == dmf::Point{1, 4},
          "single droplet did not follow the shortest route");
    router.applyFeedback({{1, first[0].from}});  // Electrode failed to move the droplet.
    check(positions.at(1) == dmf::Point{0, 4}, "failed move changed actual position");

    int steps = 0;
    while (router.hasTasks() && steps++ < 12) {
        router.setBlockages({});
        advanceAsCommanded(router);
    }
    check(!router.hasTasks() && positions.at(1) == dmf::Point{6, 4},
          "droplet did not recover after movement failure");
    check(router.costFieldBuilds() == 1,
          "unchanged blockage caused a BFS rebuild on every step");
}

void changedBlockageRebuildsOnlyWhenNeeded() {
    dmf::DropletPositions positions{{1, {0, 4}}};
    dmf::Router router(9, 9, positions);
    router.addTasks({{1, 1, {8, 4}, 1}});
    advanceAsCommanded(router);
    check(router.costFieldBuilds() == 1, "initial cost field was not built");

    const dmf::VirtualModule blocker{1, {3, 3}, 3};
    router.setBlockages({blocker});
    int steps = 0;
    while (router.hasTasks() && steps++ < 24) {
        router.setBlockages({blocker});
        const auto commands = router.planStep();
        check(commands.size() == 1, "routing task disappeared near a blockage");
        const auto point = commands[0].to;
        check(!(point.x >= 3 && point.x <= 5 && point.y >= 3 && point.y <= 5),
              "router entered a newly blocked module");
        router.applyFeedback({{1, point}});
    }
    check(!router.hasTasks(), "router did not detour around the module");
    check(router.costFieldBuilds() == 2,
          "unchanged blockage caused repeated cost-field rebuilds");

    router.setBlockages({});
    router.addTasks({{2, 1, {0, 4}, 1}});
    advanceAsCommanded(router);
    check(router.costFieldBuilds() == 3,
          "clearing a blockage did not refresh the cost field");
}

void opposingDropletsKeepTheirDistance() {
    dmf::DropletPositions positions{{1, {1, 4}}, {2, {7, 4}}};
    dmf::Router router(9, 9, positions);
    router.addTasks({{1, 1, {7, 4}, 1}, {2, 2, {1, 4}, 2}});
    int steps = 0;
    while (router.hasTasks() && steps++ < 80) {
        const auto before = positions;
        advanceAsCommanded(router);
        check(distance(positions.at(1), positions.at(2)) >= 2 &&
                  distance(positions.at(1), before.at(2)) >= 2 &&
                  distance(positions.at(2), before.at(1)) >= 2,
              "droplets violated current or previous-step separation");
    }
    check(!router.hasTasks() && positions.at(1) == dmf::Point{7, 4} &&
              positions.at(2) == dmf::Point{1, 4},
          "opposing droplets did not pass each other");
}

void invalidFeedbackLeavesStateUnchanged() {
    dmf::DropletPositions positions{{1, {0, 4}}};
    dmf::Router router(9, 9, positions);
    router.addTasks({{1, 1, {4, 4}, 1}});
    const auto command = router.planStep().at(0);
    rejects<std::invalid_argument>([&] { router.applyFeedback({{1, {8, 8}}}); },
                                   "teleporting feedback was accepted");
    check(positions.at(1) == dmf::Point{0, 4},
          "invalid feedback mutated the actual position");
    router.applyFeedback({{1, command.to}});
    check(positions.at(1) == command.to,
          "valid feedback was not accepted after invalid feedback");
}

void sharedTargetReusesOneField() {
    dmf::DropletPositions positions{{1, {0, 0}}, {2, {0, 4}}};
    dmf::Router router(9, 9, positions);
    router.addTasks({{1, 1, {8, 2}, 1}, {2, 2, {8, 2}, 2}});
    router.planStep();
    check(router.costFieldBuilds() == 1,
          "droplets with the same target built duplicate BFS fields");
}

}  // namespace

int main() {
    try {
        cachedFieldAndMovementFailure();
        changedBlockageRebuildsOnlyWhenNeeded();
        opposingDropletsKeepTheirDistance();
        invalidFeedbackLeavesStateUnchanged();
        sharedTargetReusesOneField();
        std::cout << "router tests: passed\n";
    } catch (const std::exception& error) {
        std::cerr << "router tests: " << error.what() << '\n';
        return 1;
    }
}
