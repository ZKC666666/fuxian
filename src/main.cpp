#include "dmf/dynamic_dag.hpp"

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
    dag.startOperation(mix);
    dag.finishOperation(mix, {3});
    dag.advanceCycle();

    std::cout << "cycle " << dag.cycle() << ": mix completed; detect is ready\n";
    dag.startOperation(detect);
    dag.finishOperation(detect, {4}, outcome);
    const auto activated = dag.advanceCycle();

    const auto branch = activated.at(0);
    std::cout << "cycle " << dag.cycle() << ": feedback activated "
              << dag.blockName(branch) << " block\n";
    const auto terminal = dag.operationFor(branch, outcome == "output" ? "send" : "discard");
    dag.startOperation(terminal);
    dag.finishOperation(terminal, {});
    std::cout << "all operations completed: " << std::boolalpha << dag.allDone() << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2 || (std::string(argv[1]) != "output" && std::string(argv[1]) != "waste")) {
        std::cerr << "usage: am_dmf [output|waste]\n";
        return 2;
    }
    try {
        runExample(argv[1]);
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
