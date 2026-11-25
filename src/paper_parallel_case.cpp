#include "paper_parallel_case.hpp"

#include <array>
#include <random>
#include <utility>

namespace dmf::paper {

std::optional<ParallelCase> caseForGroups(int groups) {
    switch (groups) {
        case 9: return ParallelCase{30, 4, 9};
        case 15: return ParallelCase{60, 9, 15};
        case 25: return ParallelCase{60, 9, 25};
        default: return std::nullopt;
    }
}

std::string sourceName(int group, char side) {
    return "source_" + std::to_string(group) + '_' + side;
}

std::string mixName(int group) { return "mix_" + std::to_string(group); }
std::string detectName(int group) { return "detect_" + std::to_string(group); }

Protocol makeProtocol(const ParallelCase& config) {
    BlockSpec start{"start", 0, {}, {}};
    for (int group = 0; group < config.groups; ++group) {
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

std::map<std::string, Point> sourcePorts(const ParallelCase& config) {
    std::map<std::string, Point> ports;
    const std::array<std::pair<Point, Point>, group_count> pairs{{
        {{0, 3}, {0, 5}}, {{0, 9}, {0, 11}}, {{0, 15}, {0, 17}},
        {{29, 3}, {29, 5}}, {{29, 9}, {29, 11}}, {{29, 15}, {29, 17}},
        {{9, 0}, {11, 0}}, {{15, 0}, {17, 0}}, {{9, 29}, {11, 29}}
    }};
    if (config.groups == group_count) {
        for (int group = 0; group < config.groups; ++group) {
            ports.emplace(sourceName(group, 'a'), pairs[group].first);
            ports.emplace(sourceName(group, 'b'), pairs[group].second);
        }
        return ports;
    }

    const int edge = config.chip_side - 1;
    // Spread groups along the four boundaries, aligned with module centers.
    for (int group = 0; group < config.groups; ++group) {
        const int boundary = group % 4;
        const int groups_on_edge = (config.groups + 3 - boundary) / 4;
        const int slot = (2 * (group / 4) + 1) * config.modules_per_side /
                         (2 * groups_on_edge);
        const int center = 4 + 6 * slot;
        const std::array<std::pair<Point, Point>, 4> boundary_pairs{{
            {{0, center - 1}, {0, center + 1}},
            {{edge, center - 1}, {edge, center + 1}},
            {{center - 1, 0}, {center + 1, 0}},
            {{center - 1, edge}, {center + 1, edge}}
        }};
        ports.emplace(sourceName(group, 'a'), boundary_pairs[boundary].first);
        ports.emplace(sourceName(group, 'b'), boundary_pairs[boundary].second);
    }
    return ports;
}

std::vector<std::string> outcomesFor(const ParallelCase& config,
                                     int output_percent, unsigned seed) {
    std::mt19937 engine(seed);
    std::bernoulli_distribution output(static_cast<double>(output_percent) / 100.0);
    std::vector<std::string> outcomes;
    for (int group = 0; group < config.groups; ++group) {
        outcomes.push_back(output(engine) ? "output" : "waste");
    }
    return outcomes;
}

std::map<std::string, CompletionFeedback> feedbackFor(
    const std::vector<std::string>& outcomes) {
    std::map<std::string, CompletionFeedback> feedback;
    const int groups = static_cast<int>(outcomes.size());
    for (int group = 0; group < groups; ++group) {
        feedback.emplace(sourceName(group, 'a'),
                         CompletionFeedback{{2 * group + 1}, std::nullopt});
        feedback.emplace(sourceName(group, 'b'),
                         CompletionFeedback{{2 * group + 2}, std::nullopt});
        feedback.emplace(mixName(group),
                         CompletionFeedback{{2 * groups + 1 + group}, std::nullopt});
        feedback.emplace(detectName(group),
                         CompletionFeedback{{3 * groups + 1 + group}, outcomes.at(group)});
    }
    return feedback;
}

}  // namespace dmf::paper
