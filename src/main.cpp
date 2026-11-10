#include "dmf/dynamic_dag.hpp"
#include "dmf/execution_loop.hpp"
#include "dmf/router.hpp"
#include "dmf/upper_layer.hpp"

#include <algorithm>
#include <chrono>
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

void runProtocolExample(const std::string& outcome) {
    using dmf::OperationType;
    dmf::DynamicDag dag(exampleProtocol());
    dag.activateStart({1, 2});
    dmf::DropletPositions positions{{1, {1, 2}}, {2, {1, 6}}};
    dmf::VirtualChip chip(2, 1);
    dmf::UpperLayer upper(
        dag, chip,
        {{OperationType::Mix, {3, 3, 3}}, {OperationType::Detect, {3, 3, 2}}},
        positions,
        {{OperationType::Output, {chip.width() - 1, 4}},
         {OperationType::Waste, {0, chip.height() - 1}}});
    dmf::Router router(chip.width(), chip.height(), positions);
    dmf::ExecutionLoop loop(
        dag, upper, router,
        [&dag, &outcome](dmf::OperationInstanceId operation) {
            switch (dag.operationType(operation)) {
                case OperationType::Mix: return dmf::CompletionFeedback{{3}, std::nullopt};
                case OperationType::Detect: return dmf::CompletionFeedback{{4}, outcome};
                case OperationType::Output:
                case OperationType::Waste: return dmf::CompletionFeedback{{}, std::nullopt};
                default: throw std::logic_error("unsupported operation in protocol demo");
            }
        });

    std::cout << "protocol branch=" << outcome << ", chip=" << chip.width()
              << 'x' << chip.height() << '\n';
    int steps = 0;
    while (!loop.allDone() && steps++ < 100) {
        const auto report = loop.step();
        std::cout << "step " << report.step << ": ";
        for (const auto block : report.activated_blocks) {
            std::cout << "activate " << dag.blockName(block) << " block; ";
        }
        for (const auto& task : report.routing_tasks) {
            std::cout << "route " << dag.operation(task.operation).spec_id
                      << " droplet " << task.droplet << " to ("
                      << task.target.x << ',' << task.target.y << "); ";
        }
        for (std::size_t index = 0; index < report.moves.size(); ++index) {
            const auto& move = report.moves[index];
            if (!(move.from == report.observed[index].observed)) {
                std::cout << "move " << move.droplet << " to ("
                          << move.to.x << ',' << move.to.y << "); ";
            }
        }
        for (const auto operation : report.started) {
            std::cout << "start " << dag.operation(operation).spec_id << "; ";
        }
        for (const auto operation : report.completed) {
            std::cout << "finish " << dag.operation(operation).spec_id << "; ";
        }
        std::cout << "blockages=" << report.active_blockages << '\n';
    }
    if (!loop.allDone()) throw std::runtime_error("protocol demo exceeded 100 steps");
    std::cout << "completed=" << std::boolalpha << loop.allDone()
              << ", active droplets=" << positions.size()
              << ", blocks=" << dag.blockCount()
              << ", operations=" << dag.operationCount() << "\n";
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

void runEvacuationExample() {
    using dmf::OperationType;
    dmf::BlockSpec block{
        "start", 1,
        {{"detect", OperationType::Detect, {dmf::InputRef::entry(0)}, 1}},
        std::nullopt};
    dmf::DynamicDag dag({"start", {{"start", std::move(block)}}});
    const auto start = dag.activateStart({1});
    const auto detect = dag.operationFor(start, "detect");
    dmf::DropletPositions positions{{1, {4, 3}}, {99, {4, 5}}};
    dmf::VirtualChip chip(1, 1);
    dmf::UpperLayer upper(dag, chip,
                          {{OperationType::Detect, {3, 3, 2}}}, positions);
    dmf::Router router(chip.width(), chip.height(), positions);
    dmf::ExecutionLoop loop(
        dag, upper, router,
        [](int) { return dmf::CompletionFeedback{{2}, std::nullopt}; });

    std::cout << "chip=9x9; Detect input 1=(4,3); unrelated droplet 99=(4,5)\n"
              << "reserved module x=3..5, y=3..5\n";
    for (int steps = 0; !loop.allDone() && steps < 30; ++steps) {
        const auto report = loop.step();
        std::cout << "step " << report.step << ": ";
        for (const auto& [droplet, target] : report.evacuation_targets) {
            std::cout << "evacuate " << droplet << " toward ("
                      << target.x << ',' << target.y << "); ";
        }
        for (const auto& move : report.moves) {
            if (!(move.from == move.to)) {
                std::cout << "move " << move.droplet << " to ("
                          << move.to.x << ',' << move.to.y << "); ";
            }
        }
        for (const auto droplet : report.evacuated) {
            std::cout << "droplet " << droplet << " cleared module; ";
        }
        for (const auto droplet : report.waiting_for_evacuation) {
            std::cout << "droplet " << droplet << " waiting for safe exit; ";
        }
        if (std::find(report.started.begin(), report.started.end(), detect) !=
            report.started.end()) std::cout << "start Detect; ";
        if (std::find(report.completed.begin(), report.completed.end(), detect) !=
            report.completed.end()) std::cout << "finish Detect; ";
        std::cout << "blockages=" << report.active_blockages << '\n';
    }
    if (!loop.allDone()) throw std::runtime_error("evacuation demo exceeded 30 steps");
    std::cout << "completed=" << std::boolalpha << loop.allDone()
              << ", droplet 99=(" << positions.at(99).x << ','
              << positions.at(99).y << ")\n";
}

void runCrossingExample() {
    using dmf::InputRef;
    using dmf::OperationType;
    dmf::BlockSpec block{
        "start", 2,
        {{"detect", OperationType::Detect, {InputRef::entry(0)}, 1},
         {"send", OperationType::Output, {InputRef::entry(1)}, 0}},
        std::nullopt};
    dmf::DynamicDag dag({"start", {{"start", std::move(block)}}});
    const auto instance = dag.activateStart({1, 2});
    const auto detect = dag.operationFor(instance, "detect");
    dmf::DropletPositions positions{{1, {7, 3}}, {2, {1, 4}}};
    dmf::VirtualChip chip(1, 1);
    dmf::UpperLayer upper(dag, chip,
                          {{OperationType::Detect, {3, 3, 2}}}, positions,
                          {{OperationType::Output, {8, 4}}});
    dmf::Router router(chip.width(), chip.height(), positions);
    dmf::ExecutionLoop loop(
        dag, upper, router,
        [detect](int operation) {
            return dmf::CompletionFeedback{
                operation == detect ? std::vector<int>{3} : std::vector<int>{},
                std::nullopt};
        }, {}, {{10, {3, 0}, 3}, {11, {3, 6}, 3}});

    std::cout << "chip=9x9; fixed walls x=3..5 at y=0..2 and y=6..8\n"
              << "future Detect module x=3..5, y=3..5\n"
              << "droplet 2=(1,4) -> output (8,4); Detect input 1=(7,3)\n";
    int crossing_steps = 0;
    long long crossing_check_microseconds = 0;
    for (int step = 0; step < 50 && !loop.allDone(); ++step) {
        const auto report = loop.step();
        crossing_check_microseconds += report.connectivity_check_microseconds;
        std::cout << "step " << report.step << ": ";
        if (report.crossing_modules.count(2) != 0) {
            ++crossing_steps;
            std::cout << "transfer droplet 2 before module 1 starts; ";
        }
        for (const auto& move : report.moves) {
            if (move.droplet == 2 && !(move.from == move.to)) {
                std::cout << "droplet 2 -> (" << move.to.x << ',' << move.to.y
                          << "); ";
            }
        }
        if (std::find(report.waiting_for_crossing.begin(),
                      report.waiting_for_crossing.end(), 2) !=
            report.waiting_for_crossing.end()) {
            std::cout << "waiting for safe crossing; ";
        }
        if (std::find(report.started.begin(), report.started.end(), detect) !=
            report.started.end()) std::cout << "start Detect; ";
        for (const auto operation : report.completed) {
            std::cout << "finish " << dag.operation(operation).spec_id << "; ";
        }
        std::cout << "active blockages=" << report.active_blockages << '\n';
    }
    if (!loop.allDone()) throw std::runtime_error("crossing demo exceeded 50 steps");
    std::cout << "completed=" << std::boolalpha << loop.allDone()
              << ", crossing steps=" << crossing_steps
              << ", connectivity check total (microseconds)="
              << crossing_check_microseconds << '\n';
}

void runAsyncExample() {
    using Clock = std::chrono::steady_clock;
    dmf::DropletPositions positions{{1, {0, 60}}};
    dmf::Router router(120, 120, positions);
    router.addTasks({{1, 1, {119, 60}, 0}});
    const dmf::VirtualModule blocker{1, {55, 55}, 11};
    long long planning_microseconds = 0;
    long long update_microseconds = 0;
    int route_steps = 0;
    int waiting_steps = 0;
    bool announced_stale = false;
    std::cout << "chip=120x120; droplet 1=(0,60) -> (119,60)\n";
    for (int step = 1; router.hasTasks() && step <= 300; ++step) {
        if (step == 10 || step == 40) {
            const auto start = Clock::now();
            router.setBlockages(step == 10 ?
                                   std::vector<dmf::VirtualModule>{blocker} :
                                   std::vector<dmf::VirtualModule>{});
            update_microseconds += std::chrono::duration_cast<std::chrono::microseconds>(
                                     Clock::now() - start).count();
            std::cout << "step " << step << ": blockage "
                      << (step == 10 ? "appeared" : "cleared")
                      << "; background refresh requested\n";
        }
        const int swaps_before = router.asyncFieldSwaps();
        const auto start = Clock::now();
        const auto moves = router.planStep();
        route_steps = step;
        if (moves.at(0).from == moves.at(0).to) ++waiting_steps;
        planning_microseconds += std::chrono::duration_cast<std::chrono::microseconds>(
                                   Clock::now() - start).count();
        if (router.asyncFieldSwaps() != swaps_before) {
            std::cout << "step " << step << ": new cost field activated\n";
        }
        if (!announced_stale && router.staleFieldDecisions() > 0) {
            std::cout << "step " << step
                      << ": old field guides moves; latest blockage is checked\n";
            announced_stale = true;
        }
        router.applyFeedback({{1, moves.at(0).to}});
    }
    if (router.hasTasks()) throw std::runtime_error("async demo exceeded 300 steps");
    std::cout << "arrived at (" << positions.at(1).x << ',' << positions.at(1).y << ")\n"
              << "cost fields built=" << router.costFieldBuilds()
              << ", background swaps=" << router.asyncFieldSwaps()
              << ", stale-field decisions=" << router.staleFieldDecisions() << '\n'
              << "route steps=" << route_steps
              << ", waiting steps=" << waiting_steps << '\n'
              << "planning total (microseconds)=" << planning_microseconds
              << ", blockage update total (microseconds)="
              << update_microseconds << '\n';
}

struct TimedExample {
    dmf::Point second_start;
    int duration;
    int failed_step;
};

void runTimedExample(TimedExample example) {
    dmf::DynamicDag dag(timedProtocol());
    const auto block = dag.activateStart({1, 2});
    const auto near = dag.operationFor(block, "near");
    dmf::DropletPositions positions{{1, {4, 4}}, {2, example.second_start}};
    dmf::VirtualChip chip(2, 1);
    dmf::UpperLayer upper(dag, chip,
                          {{dmf::OperationType::Detect, {3, 3, example.duration}}},
                          positions);
    dmf::Router router(chip.width(), chip.height(), positions);
    dmf::ExecutionLoop::MovementFeedbackProvider movement_feedback;
    if (example.failed_step > 0) {
        movement_feedback = [failed_step = example.failed_step](
                                const dmf::MoveCommand& command, int step) {
            return step == failed_step && command.droplet == 2
                       ? command.from : command.to;
        };
    }
    dmf::ExecutionLoop loop(
        dag, upper, router,
        [near](int operation) {
            return dmf::CompletionFeedback{{operation == near ? 3 : 4}, std::nullopt};
        },
        std::move(movement_feedback));

    std::cout << "chip=15x9, droplet 1=(4,4), droplet 2=("
              << example.second_start.x << ',' << example.second_start.y
              << "), target 2=(10,4)\n"
              << "near module: x=3..5, y=3..5; operation duration="
              << example.duration << '\n';
    if (example.failed_step > 0) {
        std::cout << "One commanded move fails at step "
                  << example.failed_step << ".\n";
    }

    long long route_time = 0;
    int steps = 0;
    while (!loop.allDone() && steps++ < 100) {
        const auto report = loop.step();
        route_time += report.routing_microseconds;
        std::cout << "step " << report.step << ": ";
        for (std::size_t i = 0; i < report.moves.size(); ++i) {
            const auto& move = report.moves[i];
            const auto actual = report.observed[i].observed;
            std::cout << "droplet " << move.droplet << " planned=("
                      << move.to.x << ',' << move.to.y << ") actual=("
                      << actual.x << ',' << actual.y << ")";
            if (!(move.from == move.to) && actual == move.from) {
                std::cout << " FAILED";
            }
            std::cout << ' ';
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
    const std::string mode = argc == 1 ? "crossing" : (argc == 2 ? argv[1] : "");
    if (mode != "crossing" && mode != "async" && mode != "evacuation" && mode != "protocol" &&
        mode != "protocol-output" && mode != "protocol-waste" &&
        mode != "detour" && mode != "timed" && mode != "router" &&
        mode != "placement" && mode != "output" && mode != "waste") {
        std::cerr << "usage: am_dmf [crossing|async|evacuation|protocol|protocol-output|protocol-waste|"
                     "detour|timed|router|placement|output|waste]\n";
        return 2;
    }
    try {
        if (mode == "crossing") runCrossingExample();
        else if (mode == "async") runAsyncExample();
        else if (mode == "evacuation") runEvacuationExample();
        else if (mode == "protocol") {
            runProtocolExample("output");
            std::cout << '\n';
            runProtocolExample("waste");
        } else if (mode == "protocol-output") runProtocolExample("output");
        else if (mode == "protocol-waste") runProtocolExample("waste");
        else if (mode == "detour") runTimedExample({{0, 4}, 20, 2});
        else if (mode == "timed") runTimedExample({{0, 7}, 3, 0});
        else if (mode == "router") runRouterExample();
        else if (mode == "placement") runPlacementExample();
        else runExample(mode);
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
