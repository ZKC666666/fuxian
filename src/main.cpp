#include "dmf/dynamic_dag.hpp"
#include "dmf/execution_loop.hpp"
#include "dmf/router.hpp"
#include "dmf/upper_layer.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

dmf::Protocol exampleProtocol() {
    using dmf::InputRef;
    using dmf::OperationType;

    dmf::BlockSpec start{
        "start", 2,
        {{"mix", OperationType::Mix, {InputRef::entry(0), InputRef::entry(1)}, 1},
         {"detect", OperationType::Detect, {InputRef::output("mix")}, 1}},
        dmf::DecisionSpec{"detect", {{"output", {"output", {0}}},
                                     {"waste", {"waste", {0}}}}}};
    dmf::BlockSpec output{"output", 1,
                          {{"send", OperationType::Output, {InputRef::entry(0)}, 0}},
                          std::nullopt};
    dmf::BlockSpec waste{"waste", 1,
                         {{"discard", OperationType::Waste, {InputRef::entry(0)}, 0}},
                         std::nullopt};
    return {"start", {{"start", std::move(start)}, {"output", std::move(output)},
                      {"waste", std::move(waste)}}};
}

void runExample(const std::string& outcome) {
    dmf::DynamicDag dag(exampleProtocol());
    const auto start = dag.activateStart({1, 2});
    const auto mix = dag.operationFor(start, "mix");
    const auto detect = dag.operationFor(start, "detect");

    std::cout << "cycle " << dag.cycle() << ": start block activated; mix is ready\n";
    dag.reserveOperation(mix);
    dag.startOperation(mix);
    dag.finishOperation(mix, {3});
    dag.advanceCycle();

    std::cout << "cycle " << dag.cycle() << ": mix completed; detect is ready\n";
    dag.reserveOperation(detect);
    dag.startOperation(detect);
    dag.finishOperation(detect, {4}, outcome);
    const auto activated = dag.advanceCycle();

    const auto branch = activated.at(0);
    std::cout << "cycle " << dag.cycle() << ": feedback activated "
              << dag.blockName(branch) << " block\n";
    const auto terminal = dag.operationFor(branch, outcome == "output" ? "send" : "discard");
    dag.reserveOperation(terminal);
    dag.startOperation(terminal);
    dag.finishOperation(terminal, {});
    std::cout << "all operations completed: " << std::boolalpha << dag.allDone() << '\n';
}

dmf::Protocol placementProtocol() {
    using dmf::InputRef;
    using dmf::OperationType;
    dmf::BlockSpec block{
        "start", 2,
        {{"mix", OperationType::Mix, {InputRef::entry(0), InputRef::entry(1)}, 1},
         {"detect", OperationType::Detect, {InputRef::output("mix")}, 1}},
        std::nullopt};
    return {"start", {{"start", std::move(block)}}};
}

void runPlacementExample() {
    using dmf::OperationType;
    dmf::DynamicDag dag(placementProtocol());
    const auto start = dag.activateStart({1, 2});
    dmf::DropletPositions positions{{1, {0, 4}}, {2, {2, 4}}};
    dmf::UpperLayer upper(dag, dmf::VirtualChip(2, 1),
                          {{OperationType::Mix, {3, 3}},
                           {OperationType::Detect, {3, 3}}},
                          positions);

    const auto mix = dag.operationFor(start, "mix");
    const auto mix_tasks = upper.placeReadyOperations();
    std::cout << "mix placed at module " << upper.moduleFor(mix).id << '\n';
    for (const auto& task : mix_tasks) {
        std::cout << "route droplet " << task.droplet << " to ("
                  << task.target.x << ',' << task.target.y << ")\n";
        positions[task.droplet] = task.target;  // v0.2-style manual arrival.
        const bool started = upper.reportArrival(task.operation, task.droplet);
        std::cout << "mix started: " << std::boolalpha << started << '\n';
    }
    std::cout << "active blockages: " << upper.activeBlockages().size() << '\n';
    upper.finishOperation(mix, {3});
    dag.advanceCycle();

    const auto detect = dag.operationFor(start, "detect");
    const auto detect_tasks = upper.placeReadyOperations();
    std::cout << "detect placed at module " << upper.moduleFor(detect).id << '\n';
    positions[detect_tasks.at(0).droplet] = detect_tasks.at(0).target;
    upper.reportArrival(detect_tasks.at(0).operation, detect_tasks.at(0).droplet);
    upper.finishOperation(detect, {4});
    dag.advanceCycle();
    std::cout << "all operations completed: " << std::boolalpha << dag.allDone() << '\n';
}

void routeUntilRunning(dmf::DynamicDag& dag, dmf::UpperLayer& upper,
                       dmf::Router& router, dmf::OperationInstanceId operation) {
    const auto tasks = upper.placeReadyOperations();
    for (const auto& task : tasks) {
        std::cout << "route droplet " << task.droplet << " to ("
                  << task.target.x << ',' << task.target.y << ")\n";
    }
    for (const auto& arrival : router.addTasks(tasks)) {
        upper.reportArrival(arrival.operation, arrival.droplet);
    }
    for (int step = 1; step <= 100 &&
                       dag.operation(operation).state != dmf::OperationState::Running; ++step) {
        router.setBlockages(upper.activeBlockages());
        const auto commands = router.planStep();
        if (commands.empty()) throw std::runtime_error("routing made no progress");
        std::vector<dmf::MoveFeedback> feedback;
        for (const auto& command : commands) {
            feedback.push_back({command.droplet, command.to});
            if (!(command.from == command.to)) {
                std::cout << "step " << step << ": droplet " << command.droplet
                          << " -> (" << command.to.x << ',' << command.to.y << ")\n";
            }
        }
        for (const auto& arrival : router.applyFeedback(feedback)) {
            upper.reportArrival(arrival.operation, arrival.droplet);
        }
        upper.tryStartPlacedOperations();
    }
    if (dag.operation(operation).state != dmf::OperationState::Running) {
        throw std::runtime_error("routing did not start the operation within 100 steps");
    }
    router.setBlockages(upper.activeBlockages());
}

void runRouterExample() {
    using dmf::OperationType;
    dmf::DynamicDag dag(placementProtocol());
    const auto start = dag.activateStart({1, 2});
    dmf::DropletPositions positions{{1, {0, 4}}, {2, {2, 4}}};
    dmf::VirtualChip chip(2, 1);
    dmf::UpperLayer upper(dag, chip,
                          {{OperationType::Mix, {3, 3}},
                           {OperationType::Detect, {3, 3}}}, positions);
    dmf::Router router(chip.width(), chip.height(), positions);

    const auto mix = dag.operationFor(start, "mix");
    routeUntilRunning(dag, upper, router, mix);
    std::cout << "mix started after physical arrival\n";
    upper.finishOperation(mix, {3});  // Operation timing is not modeled yet.
    router.setBlockages(upper.activeBlockages());
    dag.advanceCycle();

    const auto detect = dag.operationFor(start, "detect");
    routeUntilRunning(dag, upper, router, detect);
    std::cout << "detect started after physical arrival\n";
    upper.finishOperation(detect, {4});
    dag.advanceCycle();
    std::cout << "all operations completed: " << std::boolalpha << dag.allDone() << '\n';
    std::cout << "cost fields built: " << router.costFieldBuilds() << '\n';
}

dmf::Protocol timedProtocol() {
    using dmf::InputRef;
    using dmf::OperationType;
    dmf::BlockSpec block{
        "start", 2,
        {{"near", OperationType::Detect, {InputRef::entry(0)}, 1},
         {"far", OperationType::Detect, {InputRef::entry(1)}, 1}},
        std::nullopt};
    return {"start", {{"start", std::move(block)}}};
}

void runTimedExample() {
    dmf::DynamicDag dag(timedProtocol());
    const auto block = dag.activateStart({1, 2});
    const auto near = dag.operationFor(block, "near");
    dmf::DropletPositions positions{{1, {4, 4}}, {2, {0, 7}}};
    dmf::VirtualChip chip(2, 1);
    dmf::UpperLayer upper(dag, chip,
                          {{dmf::OperationType::Detect, {3, 3, 3}}}, positions);
    dmf::Router router(chip.width(), chip.height(), positions);
    dmf::ExecutionLoop loop(dag, upper, router, [near](int operation) {
        return dmf::CompletionFeedback{{operation == near ? 3 : 4}, std::nullopt};
    });

    long long route_time = 0;
    int steps = 0;
    while (!loop.allDone() && steps++ < 100) {
        const auto report = loop.step();
        route_time += report.routing_microseconds;
        std::cout << "step " << report.step << ": ";
        for (const auto& move : report.moves) {
            if (!(move.from == move.to)) {
                std::cout << "droplet " << move.droplet << " -> ("
                          << move.to.x << ',' << move.to.y << ") ";
            }
        }
        for (const auto operation : report.started) {
            std::cout << "start " << (operation == near ? "near" : "far") << ' ';
        }
        for (const auto operation : report.completed) {
            std::cout << "finish " << (operation == near ? "near" : "far") << ' ';
        }
        std::cout << "blockages=" << report.active_blockages << '\n';
    }
    if (!loop.allDone()) throw std::runtime_error("timed example exceeded 100 steps");
    std::cout << "all operations completed: " << std::boolalpha << loop.allDone() << '\n';
    std::cout << "cost fields built: " << router.costFieldBuilds() << '\n';
    std::cout << "router planning total (microseconds, excluding output): "
              << route_time << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    const std::string mode = argc == 1 ? "timed" : (argc == 2 ? argv[1] : "");
    if (mode != "timed" && mode != "router" && mode != "placement" &&
        mode != "output" && mode != "waste") {
        std::cerr << "usage: am_dmf [timed|router|placement|output|waste]\n";
        return 2;
    }
    try {
        if (mode == "timed") runTimedExample();
        else if (mode == "router") runRouterExample();
        else if (mode == "placement") runPlacementExample();
        else runExample(mode);
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
