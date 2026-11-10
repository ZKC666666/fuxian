#include "dmf/execution_loop.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

dmf::Protocol branchingProtocol() {
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

bool contains(const std::vector<int>& values, int value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

void checkSeparation(const dmf::DropletPositions& positions) {
    for (auto left = positions.begin(); left != positions.end(); ++left) {
        for (auto right = std::next(left); right != positions.end(); ++right) {
            const int dx = std::abs(left->second.x - right->second.x);
            const int dy = std::abs(left->second.y - right->second.y);
            check(std::max(dx, dy) >= 2, "droplet separation was violated");
        }
    }
}

void completesSelectedBranch(const std::string& outcome) {
    using dmf::OperationType;
    dmf::DynamicDag dag(branchingProtocol());
    const auto start = dag.activateStart({1, 2});
    const auto mix = dag.operationFor(start, "mix");
    const auto detect = dag.operationFor(start, "detect");
    dmf::DropletPositions positions{{1, {1, 2}}, {2, {1, 6}}};
    dmf::VirtualChip chip(2, 1);
    const dmf::Point output_port{chip.width() - 1, 4};
    const dmf::Point waste_port{0, chip.height() - 1};
    dmf::UpperLayer upper(
        dag, chip,
        {{OperationType::Mix, {3, 3, 3}}, {OperationType::Detect, {3, 3, 2}}},
        positions,
        {{OperationType::Output, output_port}, {OperationType::Waste, waste_port}});
    dmf::Router router(chip.width(), chip.height(), positions);
    dmf::ExecutionLoop loop(
        dag, upper, router,
        [&dag, &outcome](int operation) {
            switch (dag.operationType(operation)) {
                case OperationType::Mix: return dmf::CompletionFeedback{{3}, std::nullopt};
                case OperationType::Detect: return dmf::CompletionFeedback{{4}, outcome};
                case OperationType::Output:
                case OperationType::Waste: return dmf::CompletionFeedback{{}, std::nullopt};
                default: throw std::logic_error("unexpected operation");
            }
        });

    int mix_finish = 0;
    int detect_start = 0;
    int detect_finish = 0;
    int branch_step = 0;
    int terminal_start = 0;
    int terminal_finish = 0;
    dmf::OperationInstanceId terminal = 0;
    while (!loop.allDone() && dag.cycle() < 60) {
        const auto report = loop.step();
        checkSeparation(positions);
        check(report.moves.size() == report.observed.size(),
              "planned and observed moves differ");
        if (contains(report.completed, mix)) mix_finish = report.step;
        if (contains(report.completed, mix)) {
            check(positions.count(1) == 0 && positions.count(2) == 0 &&
                      positions.at(3) == dmf::Point{4, 4},
                  "mix did not replace its inputs with one output droplet");
        }
        if (contains(report.started, detect)) detect_start = report.step;
        if (contains(report.completed, detect)) detect_finish = report.step;
        if (contains(report.completed, detect)) {
            check(positions.count(3) == 0 && positions.at(4) == dmf::Point{4, 4},
                  "detect output position was wrong");
        }
        for (const auto block : report.activated_blocks) {
            check(terminal == 0 && dag.blockName(block) == outcome,
                  "wrong or duplicate branch was activated");
            branch_step = report.step;
            terminal = dag.operationFor(block, outcome == "output" ? "send" : "discard");
        }
        for (const auto& task : report.routing_tasks) {
            if (terminal != 0 && task.operation == terminal) {
                check(task.droplet == 4 && task.module_id == 0 &&
                          task.target ==
                              (outcome == "output" ? output_port : waste_port),
                      "terminal routing task did not target its fixed port");
            }
        }
        if (terminal != 0 && contains(report.started, terminal)) {
            terminal_start = report.step;
            check(positions.at(4) ==
                      (outcome == "output" ? output_port : waste_port),
                  "terminal operation started before reaching its port");
            check(report.active_blockages == 0,
                  "terminal operation occupied a virtual module");
        }
        if (terminal != 0 && contains(report.completed, terminal)) {
            terminal_finish = report.step;
            check(positions.empty(), "terminal operation did not consume its droplet");
        }
        if (detect_finish == 0) {
            check(terminal == 0, "branch activated before detection completed");
        }
    }
    check(loop.allDone() && dag.blockCount() == 2 && dag.operationCount() == 3,
          "full protocol did not finish exactly one branch");
    check(mix_finish > 0 && detect_start > mix_finish &&
              detect_finish > detect_start && branch_step == detect_finish + 1,
          "dependency or branch timing was wrong");
    check(terminal_start >= branch_step && terminal_finish > terminal_start,
          "terminal routing or completion timing was wrong");
}

void rejectsInvalidPorts() {
    using dmf::OperationType;
    dmf::DynamicDag dag(branchingProtocol());
    dmf::DropletPositions positions{{1, {1, 2}}, {2, {1, 6}}};
    dmf::VirtualChip chip(2, 1);
    try {
        dmf::UpperLayer upper(dag, chip, {}, positions,
                              {{OperationType::Output, {4, 4}}});
        throw std::runtime_error("interior output port was accepted");
    } catch (const std::invalid_argument&) {
    }
    try {
        dmf::UpperLayer upper(dag, chip, {}, positions,
                              {{OperationType::Output, {0, 0}},
                               {OperationType::Waste, {0, 0}}});
        throw std::runtime_error("shared output and waste port was accepted");
    } catch (const std::invalid_argument&) {
    }

    dmf::BlockSpec terminal{"start", 1,
                            {{"send", OperationType::Output,
                              {dmf::InputRef::entry(0)}, 0}},
                            std::nullopt};
    dmf::DynamicDag terminal_dag({"start", {{"start", std::move(terminal)}}});
    const auto block = terminal_dag.activateStart({1});
    dmf::DropletPositions one_droplet{{1, {0, 0}}};
    dmf::UpperLayer without_port(terminal_dag, chip, {}, one_droplet);
    try {
        without_port.placeReadyOperations();
        throw std::runtime_error("missing terminal port was accepted");
    } catch (const std::logic_error&) {
    }
    check(terminal_dag.operation(terminal_dag.operationFor(block, "send")).state ==
              dmf::OperationState::Waiting,
          "missing port changed operation state");
}

}  // namespace

int main() {
    try {
        completesSelectedBranch("output");
        completesSelectedBranch("waste");
        rejectsInvalidPorts();
        std::cout << "protocol execution tests: passed\n";
    } catch (const std::exception& error) {
        std::cerr << "protocol execution tests: " << error.what() << '\n';
        return 1;
    }
}
