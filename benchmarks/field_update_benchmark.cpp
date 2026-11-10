#include "dmf/router.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

long long microseconds(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               Clock::now() - start).count();
}

struct Result {
    long long add_decision_us;
    long long remove_decision_us;
    long long add_notification_us;
    long long remove_notification_us;
    int route_steps;
    int waiting_steps;
};

Result oneRun() {
    dmf::DropletPositions positions{{1, {0, 60}}};
    dmf::Router router(120, 120, positions);
    router.addTasks({{1, 1, {119, 60}, 0}});
    const dmf::VirtualModule blocker{1, {55, 55}, 11};
    Result result{};
    for (int step = 1; router.hasTasks() && step <= 300; ++step) {
        if (step == 10 || step == 40) {
            const auto start = Clock::now();
            router.setBlockages(step == 10 ?
                                   std::vector<dmf::VirtualModule>{blocker} :
                                   std::vector<dmf::VirtualModule>{});
            const auto elapsed = microseconds(start);
            if (step == 10) result.add_notification_us = elapsed;
            else result.remove_notification_us = elapsed;
        }
        const auto start = Clock::now();
        const auto move = router.planStep().at(0);
        const auto elapsed = microseconds(start);
        if (step == 10) result.add_decision_us = elapsed;
        if (step == 40) result.remove_decision_us = elapsed;
        if (move.from == move.to) ++result.waiting_steps;
        router.applyFeedback({{1, move.to}});
        result.route_steps = step;
    }
    if (router.hasTasks() || !(positions.at(1) == dmf::Point{119, 60})) {
        throw std::runtime_error("benchmark route did not complete");
    }
    return result;
}

template <typename Member>
long long median(const std::vector<Result>& results, Member member) {
    std::vector<long long> values;
    for (const auto& result : results) values.push_back(result.*member);
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

}  // namespace

int main() {
    try {
        std::vector<Result> results;
        for (int run = 0; run < 10; ++run) results.push_back(oneRun());
        std::cout << "runs=10, chip=120x120, one droplet, blockage at step 10,"
                     " clear at step 40\n"
                  << "median add notification (us)="
                  << median(results, &Result::add_notification_us) << '\n'
                  << "median first decision after add (us)="
                  << median(results, &Result::add_decision_us) << '\n'
                  << "median remove notification (us)="
                  << median(results, &Result::remove_notification_us) << '\n'
                  << "median first decision after remove (us)="
                  << median(results, &Result::remove_decision_us) << '\n'
                  << "median route steps=" << median(results, &Result::route_steps) << '\n'
                  << "median waiting steps=" << median(results, &Result::waiting_steps)
                  << '\n';
    } catch (const std::exception& error) {
        std::cerr << "field benchmark: " << error.what() << '\n';
        return 1;
    }
}
