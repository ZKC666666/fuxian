#include "paper_parallel_case.hpp"

#include <array>
#include <random>
#include <utility>

namespace dmf::paper {

std::string sourceName(int group, char side) {
    return "source_" + std::to_string(group) + '_' + side;
}

std::string mixName(int group) { return "mix_" + std::to_string(group); }
std::string detectName(int group) { return "detect_" + std::to_string(group); }

Protocol makeProtocol() {
    BlockSpec start{"start", 0, {}, {}};
    for (int group = 0; group < group_count; ++group) {
        const auto a = sourceName(group, 'a');
        const auto b = sourceName(group, 'b');
        const auto mix = mixName(group);
        const auto detect = detectName(group);
        start.operations.push_back({a, OperationType::Dispense, {}, 1});
        start.operations.push_back({b, OperationType::Dispense, {}, 1});
        start.operations.push_back({mix, OperationType::Mix,
                                    {InputRef::output(a), InputRef::output(b)}, 1});
        start.operations.push_back({detect, OperationType::Detect,
                                    {InputRef::output(mix)}, 1});
        start.decisions.push_back({detect, {{"output", {"output", {0}}},
                                           {"waste", {"waste", {0}}}}});
    }
    BlockSpec output{"output", 1,
                     {{"finish", OperationType::Output, {InputRef::entry(0)}, 0}}, {}};
    BlockSpec waste{"waste", 1,
                    {{"finish", OperationType::Waste, {InputRef::entry(0)}, 0}}, {}};
    return {"start", {{"start", std::move(start)},
                      {"output", std::move(output)},
                      {"waste", std::move(waste)}}};
}

std::map<std::string, Point> sourcePorts() {
    std::map<std::string, Point> ports;
    const std::array<std::pair<Point, Point>, group_count> pairs{{
        {{0, 3}, {0, 5}}, {{0, 9}, {0, 11}}, {{0, 15}, {0, 17}},
        {{29, 3}, {29, 5}}, {{29, 9}, {29, 11}}, {{29, 15}, {29, 17}},
        {{9, 0}, {11, 0}}, {{15, 0}, {17, 0}}, {{9, 29}, {11, 29}}
    }};
    for (int group = 0; group < group_count; ++group) {
        ports.emplace(sourceName(group, 'a'), pairs[group].first);
        ports.emplace(sourceName(group, 'b'), pairs[group].second);
    }
    return ports;
}

std::vector<std::string> outcomesFor(int output_percent, unsigned seed) {
    std::mt19937 engine(seed);
    std::bernoulli_distribution output(static_cast<double>(output_percent) / 100.0);
    std::vector<std::string> outcomes;
    for (int group = 0; group < group_count; ++group) {
        outcomes.push_back(output(engine) ? "output" : "waste");
    }
    return outcomes;
}

std::map<std::string, CompletionFeedback> feedbackFor(
    const std::vector<std::string>& outcomes) {
    std::map<std::string, CompletionFeedback> feedback;
    for (int group = 0; group < group_count; ++group) {
        feedback.emplace(sourceName(group, 'a'),
                         CompletionFeedback{{2 * group + 1}, std::nullopt});
        feedback.emplace(sourceName(group, 'b'),
                         CompletionFeedback{{2 * group + 2}, std::nullopt});
        feedback.emplace(mixName(group),
                         CompletionFeedback{{19 + group}, std::nullopt});
        feedback.emplace(detectName(group),
                         CompletionFeedback{{28 + group}, outcomes.at(group)});
    }
    return feedback;
}

}  // namespace dmf::paper
