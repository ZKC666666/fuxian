#include "dmf/dynamic_dag.hpp"
#include "dmf/upper_layer.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

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

void runPlacementExample() {
    using dmf::InputRef;
    using dmf::OperationType;
    dmf::BlockSpec block{
        "start", 2,
        {{"mix", OperationType::Mix, {InputRef::entry(0), InputRef::entry(1)}, 1},
         {"detect", OperationType::Detect, {InputRef::output("mix")}, 1}},
        std::nullopt};
    dmf::DynamicDag dag({"start", {{"start", std::move(block)}}});
    const auto start = dag.activateStart({1, 2});
    dmf::UpperLayer upper(dag, dmf::VirtualChip(2, 1),
                          {{OperationType::Mix, {3, 3}},
                           {OperationType::Detect, {3, 3}}},
                          {{1, {0, 4}}, {2, {2, 4}}});

    const auto mix = dag.operationFor(start, "mix");
    const auto mix_tasks = upper.placeReadyOperations();
    std::cout << "mix placed at module " << upper.moduleFor(mix).id << '\n';
    for (const auto& task : mix_tasks) {
        std::cout << "route droplet " << task.droplet << " to ("
                  << task.target.x << ',' << task.target.y << ")\n";
        const bool started = upper.reportArrival(task.operation, task.droplet);
        std::cout << "mix started: " << std::boolalpha << started << '\n';
    }
    std::cout << "active blockages: " << upper.activeBlockages().size() << '\n';
    upper.finishOperation(mix, {3});
    dag.advanceCycle();

    const auto detect = dag.operationFor(start, "detect");
    const auto detect_tasks = upper.placeReadyOperations();
    std::cout << "detect placed at module " << upper.moduleFor(detect).id << '\n';
    upper.reportArrival(detect_tasks.at(0).operation, detect_tasks.at(0).droplet);
    upper.finishOperation(detect, {4});
    dag.advanceCycle();
    std::cout << "all operations completed: " << std::boolalpha << dag.allDone() << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    const std::string mode = argc == 1 ? "placement" : (argc == 2 ? argv[1] : "");
    if (mode != "placement" && mode != "output" && mode != "waste") {
        std::cerr << "usage: am_dmf [placement|output|waste]\n";
        return 2;
    }
    try {
        if (mode == "placement") runPlacementExample();
        else runExample(mode);
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
