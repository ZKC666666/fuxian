#include "dmf/cost_field_updater.hpp"
#include "dmf/router.hpp"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void latestSnapshotWins() {
    dmf::CostFieldUpdater updater;
    auto clear = std::make_shared<dmf::CostFieldSnapshot>(
        dmf::CostFieldSnapshot{9, 9, 1, std::vector<unsigned char>(81, 0),
                               std::vector<int>(81, 0)});
    auto blocked = std::make_shared<dmf::CostFieldSnapshot>(*clear);
    blocked->version = 2;
    for (int y = 3; y <= 5; ++y) {
        for (int x = 3; x <= 5; ++x) blocked->blocked[y * 9 + x] = 1;
    }
    const dmf::CostFieldKey target{4 * 9 + 8, 0};
    updater.schedule(target, clear);
    updater.schedule(target, blocked);
    auto latest = updater.wait(target, 2);
    check(latest && latest->at(4 * 9) > 8,
          "latest blockage was missing from the background field");
    check(!updater.take(target, 1), "obsolete field was published after a newer request");
}

void staleFieldNeverOverridesHardBlockage() {
    dmf::DropletPositions positions{{1, {0, 4}}};
    dmf::Router router(9, 9, positions);
    router.addTasks({{1, 1, {8, 4}, 0}});
    const auto first = router.planStep().at(0);
    router.applyFeedback({{1, first.to}});
    check(router.costFieldBuilds() == 1, "initial field was not built once");

    const dmf::VirtualModule blocker{1, {3, 3}, 3};
    router.setBlockages({blocker});
    check(router.costFieldBuilds() == 1,
          "blockage update synchronously rebuilt the cost field");
    int steps = 0;
    while (router.hasTasks() && steps++ < 40) {
        const auto moves = router.planStep();
        check(moves.size() == 1 &&
                  (moves[0].to.x < 3 || moves[0].to.x > 5 ||
                   moves[0].to.y < 3 || moves[0].to.y > 5),
              "routing entered a new blockage while using a stale field");
        router.applyFeedback({{1, moves[0].to}});
        if (steps == 1) router.waitForCostFieldUpdates();
    }
    check(!router.hasTasks() && positions.at(1) == dmf::Point{8, 4} &&
              router.asyncFieldSwaps() == 1,
          "router did not accept the background field and finish the route");
}

void rapidUpdatesDiscardOldGenerations() {
    dmf::DropletPositions positions{{1, {0, 4}}};
    dmf::Router router(9, 9, positions);
    router.addTasks({{1, 1, {8, 4}, 0}});
    const auto first = router.planStep().at(0);
    router.applyFeedback({{1, first.to}});

    router.setBlockages({{1, {3, 3}, 3}});
    router.setBlockages({{2, {3, 0}, 3}});
    router.setBlockages({});
    router.waitForCostFieldUpdates();
    check(router.costFieldBuilds() == 2 && router.asyncFieldSwaps() == 1,
          "several changes installed an intermediate cost field");
    const auto next = router.planStep().at(0);
    check(next.to == dmf::Point{2, 4},
          "latest empty-blockage field did not restore the straight route");
    router.applyFeedback({{1, next.to}});
}

void sharedTargetReusesOneBackgroundJob() {
    dmf::DropletPositions positions{{1, {0, 0}}, {2, {0, 4}}};
    dmf::Router router(9, 9, positions);
    router.addTasks({{1, 1, {8, 2}, 0}, {2, 2, {8, 2}, 0}});
    const auto moves = router.planStep();
    std::vector<dmf::MoveFeedback> feedback;
    for (const auto& move : moves) feedback.push_back({move.droplet, move.to});
    router.applyFeedback(feedback);
    check(router.costFieldBuilds() == 1, "shared target built duplicate initial fields");
    router.setBlockages({{1, {3, 3}, 3}});
    router.waitForCostFieldUpdates();
    check(router.costFieldBuilds() == 2 && router.asyncFieldSwaps() == 1,
          "shared target built duplicate background fields");
}

}  // namespace

int main() {
    try {
        latestSnapshotWins();
        staleFieldNeverOverridesHardBlockage();
        rapidUpdatesDiscardOldGenerations();
        sharedTargetReusesOneBackgroundJob();
        std::cout << "async cost field tests: passed\n";
    } catch (const std::exception& error) {
        std::cerr << "async cost field tests: " << error.what() << '\n';
        return 1;
    }
}
