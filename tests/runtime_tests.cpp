#include "dmf/dynamic_dag.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
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

bool contains(const std::vector<dmf::OperationInstanceId>& ids, dmf::OperationInstanceId id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

dmf::Protocol branchingProtocol() {
    using dmf::InputRef;
    using dmf::OperationType;

    dmf::BlockSpec start{
        "start", 2,
        {{"mix", OperationType::Mix, {InputRef::entry(0), InputRef::entry(1)}, 1},
         {"detect", OperationType::Detect, {InputRef::output("mix")}, 1}},
        {dmf::DecisionSpec{"detect", {{"output", {"output", {0}}},
                                      {"waste", {"waste", {0}}}}}}};
    dmf::BlockSpec output{"output", 1,
                          {{"send", OperationType::Output, {InputRef::entry(0)}, 0}},
                          {}};
    dmf::BlockSpec waste{"waste", 1,
                         {{"discard", OperationType::Waste, {InputRef::entry(0)}, 0}},
                         {}};
    return {"start", {{"start", std::move(start)}, {"output", std::move(output)},
                      {"waste", std::move(waste)}}};
}

void followsOnlyTheObservedBranch(const std::string& outcome) {
    dmf::DynamicDag dag(branchingProtocol());
    const auto start = dag.activateStart({1, 2});
    const auto mix = dag.operationFor(start, "mix");
    const auto detect = dag.operationFor(start, "detect");

    check(dag.blockCount() == 1 && dag.operationCount() == 2,
          "future branches were activated before feedback");
    check(contains(dag.readyOperations(), mix) && !contains(dag.readyOperations(), detect),
          "operation dependency was ignored");

    check(dag.reserveOperation(mix) == std::vector<dmf::DropletId>({1, 2}),
          "mix received wrong droplets");
    dag.startOperation(mix);
    dag.finishOperation(mix, {3});
    check(!contains(dag.readyOperations(), detect),
          "same-cycle output became ready before the next cycle");
    dag.advanceCycle();
    check(contains(dag.readyOperations(), detect), "detect did not become ready");

    check(dag.reserveOperation(detect) == std::vector<dmf::DropletId>({3}),
          "detect received wrong droplet");
    dag.startOperation(detect);
    dag.finishOperation(detect, {4}, outcome);
    check(dag.blockCount() == 1 && !dag.allDone(),
          "branch was activated before the next cycle");
    const auto activated = dag.advanceCycle();

    check(dag.blockCount() == 2 && dag.operationCount() == 3,
          "exactly one branch should be activated");
    check(activated.size() == 1, "decision should activate one block");
    const auto branch = activated.front();
    check(dag.blockName(branch) == outcome, "wrong branch was selected");
    const auto terminal = dag.operationFor(branch, outcome == "output" ? "send" : "discard");
    check(contains(dag.readyOperations(), terminal), "branch input was not transferred");
    check(dag.reserveOperation(terminal) == std::vector<dmf::DropletId>({4}),
          "branch received wrong droplet");
    dag.startOperation(terminal);
    dag.finishOperation(terminal, {});
    check(dag.allDone(), "completed path should finish");
}

void repeatedBlockGetsNewOperationIds() {
    using dmf::InputRef;
    using dmf::OperationType;

    dmf::BlockSpec check_block{
        "check", 1,
        {{"detect", OperationType::Detect, {InputRef::entry(0)}, 1}},
        {dmf::DecisionSpec{"detect", {{"retry", {"check", {0}}},
                                      {"finish", {"done", {0}}}}}}};
    dmf::BlockSpec done{"done", 1,
                        {{"send", OperationType::Output, {InputRef::entry(0)}, 0}},
                        {}};
    dmf::DynamicDag dag({"check", {{"check", std::move(check_block)},
                                   {"done", std::move(done)}}});

    const auto first_block = dag.activateStart({10});
    const auto first_op = dag.operationFor(first_block, "detect");
    dag.reserveOperation(first_op);
    dag.startOperation(first_op);
    dag.finishOperation(first_op, {11}, "retry");
    const auto retry_blocks = dag.advanceCycle();

    check(retry_blocks.size() == 1, "retry should activate one block");
    const auto second_block = retry_blocks.front();
    const auto second_op = dag.operationFor(second_block, "detect");
    check(first_block != second_block && first_op != second_op,
          "repeated CFG block reused a runtime operation id");
    check(contains(dag.readyOperations(), second_op), "retry block did not become ready");
    dag.reserveOperation(second_op);
    dag.startOperation(second_op);
    dag.finishOperation(second_op, {12}, "finish");
    const auto final_blocks = dag.advanceCycle();

    check(final_blocks.size() == 1, "finish should activate one block");
    const auto terminal = dag.operationFor(final_blocks.front(), "send");
    check(dag.reserveOperation(terminal) == std::vector<dmf::DropletId>({12}),
          "final block did not receive the newest droplet");
    dag.startOperation(terminal);
    dag.finishOperation(terminal, {});
    check(dag.allDone() && dag.blockCount() == 3, "retry path did not finish");
}

void invalidGraphsAreRejected() {
    using dmf::InputRef;
    using dmf::OperationType;
    dmf::BlockSpec cyclic{
        "cycle", 0,
        {{"a", OperationType::Mix, {InputRef::output("b")}, 1},
         {"b", OperationType::Mix, {InputRef::output("a")}, 1}},
        {}};
    rejects<std::invalid_argument>(
        [&] { dmf::DynamicDag dag({"cycle", {{"cycle", cyclic}}}); },
        "cyclic block DAG was accepted");

    auto protocol = branchingProtocol();
    protocol.blocks.at("start").decisions.front().outcomes.at("output").target_block = "missing";
    rejects<std::invalid_argument>([&] { dmf::DynamicDag dag(protocol); },
                                   "missing CFG target was accepted");

    auto duplicate_input = branchingProtocol();
    duplicate_input.blocks.at("start").operations.at(0).inputs =
        {InputRef::entry(0), InputRef::entry(0)};
    rejects<std::invalid_argument>(
        [&] { dmf::DynamicDag dag(duplicate_input); },
        "operation using one droplet twice was accepted");
}

void oneDropletCannotStartTwoOperations() {
    using dmf::InputRef;
    using dmf::OperationType;
    dmf::BlockSpec block{
        "start", 1,
        {{"first", OperationType::Output, {InputRef::entry(0)}, 0},
         {"second", OperationType::Waste, {InputRef::entry(0)}, 0}},
        {}};
    dmf::DynamicDag dag({"start", {{"start", std::move(block)}}});
    const auto start = dag.activateStart({1});
    const auto first = dag.operationFor(start, "first");
    const auto second = dag.operationFor(start, "second");
    check(contains(dag.readyOperations(), first) && contains(dag.readyOperations(), second),
          "both operations should initially see the droplet");
    dag.reserveOperation(first);
    check(!contains(dag.readyOperations(), second), "reserved droplet was reused");
    rejects<std::logic_error>([&] { dag.reserveOperation(second); },
                              "second operation reused a consumed droplet");
}

void badFeedbackDoesNotFinishAnOperation() {
    dmf::DynamicDag dag(branchingProtocol());
    const auto start = dag.activateStart({1, 2});
    const auto mix = dag.operationFor(start, "mix");
    dag.reserveOperation(mix);
    dag.startOperation(mix);
    rejects<std::invalid_argument>([&] { dag.finishOperation(mix, {1}); },
                                   "existing droplet id was accepted");
    check(dag.operation(mix).state == dmf::OperationState::Running,
          "invalid output changed operation state");
    dag.finishOperation(mix, {3});
    dag.advanceCycle();

    const auto detect = dag.operationFor(start, "detect");
    dag.reserveOperation(detect);
    dag.startOperation(detect);
    rejects<std::invalid_argument>([&] { dag.finishOperation(detect, {4}, "unknown"); },
                                   "unknown detection result was accepted");
    check(dag.operation(detect).state == dmf::OperationState::Running,
          "invalid decision changed operation state");
}

}  // namespace

int main() {
    try {
        followsOnlyTheObservedBranch("output");
        followsOnlyTheObservedBranch("waste");
        repeatedBlockGetsNewOperationIds();
        invalidGraphsAreRejected();
        oneDropletCannotStartTwoOperations();
        badFeedbackDoesNotFinishAnOperation();
        std::cout << "runtime tests: passed\n";
    } catch (const std::exception& error) {
        std::cerr << "runtime tests: " << error.what() << '\n';
        return 1;
    }
}
