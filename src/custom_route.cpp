#include "custom_route_case.hpp"

#include "dmf/router.hpp"

#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int runCase(const dmf::demo::RouteCase& experiment) {
    auto positions = experiment.positions;
    dmf::Router router(experiment.width, experiment.height,
                       positions, experiment.separation);
    std::set<dmf::DropletId> arrived;
    const auto markArrivals = [&](const std::vector<dmf::RouteArrival>& events) {
        for (const auto& event : events) {
            arrived.insert(event.droplet);
            std::cout << "arrived: droplet " << event.droplet << '\n';
        }
    };
    markArrivals(router.addTasks(experiment.tasks));

    int steps = 0;
    int stagnant_steps = 0;
    std::string stop_reason;
    while (router.hasTasks() && steps < experiment.max_steps) {
        ++steps;
        try {
            const auto commands = router.planStep();
            bool moved = false;
            std::vector<dmf::MoveFeedback> feedback;
            feedback.reserve(commands.size());
            for (const auto& command : commands) {
                feedback.push_back({command.droplet, command.to});
                moved = moved || !(command.from == command.to);
                if (experiment.trace) {
                    std::cout << "step " << steps << ": droplet " << command.droplet
                              << " (" << command.from.x << ',' << command.from.y << ") -> ("
                              << command.to.x << ',' << command.to.y << ")\n";
                }
            }
            markArrivals(router.applyFeedback(feedback));
            stagnant_steps = moved ? 0 : stagnant_steps + 1;
            if (stagnant_steps >= 10 && router.hasTasks()) {
                stop_reason = "no droplet moved for 10 consecutive steps";
                break;
            }
        } catch (const std::exception& error) {
            stop_reason = error.what();
            break;
        }
    }

    std::cout << "steps: " << steps << ", arrived: " << arrived.size()
              << '/' << experiment.tasks.size() << '\n';
    std::cout << "cost fields built: " << router.costFieldBuilds() << '\n';
    if (!stop_reason.empty()) std::cout << "stopped: " << stop_reason << '\n';
    if (arrived.size() == experiment.tasks.size()) {
        std::cout << "result: all droplets arrived\n";
        return 0;
    }
    for (const auto& task : experiment.tasks) {
        if (arrived.count(task.droplet) != 0) continue;
        const auto position = positions.at(task.droplet);
        std::cout << "unfinished droplet " << task.droplet << ": at ("
                  << position.x << ',' << position.y << "), target ("
                  << task.target.x << ',' << task.target.y << ")\n";
    }
    std::cout << (stop_reason.empty() ? "result: step limit reached\n"
                                      : "result: routing stopped before completion\n");
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 2) {
        std::cerr << "usage: dmf_custom_route [case_file]\n";
        return 2;
    }
    const std::string path = argc == 2 ? argv[1] : DMF_DEFAULT_CASE_PATH;
    std::ifstream input(path);
    if (!input) {
        std::cerr << "could not open routing case: " << path << '\n';
        return 2;
    }
    try {
        std::cout << "case: " << path << '\n';
        return runCase(dmf::demo::parseRouteCase(input));
    } catch (const std::exception& error) {
        std::cerr << "invalid routing case: " << error.what() << '\n';
        return 2;
    }
}
