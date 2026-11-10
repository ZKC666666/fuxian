#include "dmf/cost_field_updater.hpp"

#include <algorithm>
#include <limits>
#include <queue>
#include <stdexcept>

namespace dmf {

namespace {
constexpr int unreachable = std::numeric_limits<int>::max();
}

CostFieldUpdater::CostFieldUpdater() : worker_(&CostFieldUpdater::run, this) {}

CostFieldUpdater::~CostFieldUpdater() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    work_ready_.notify_one();
    result_ready_.notify_all();
    worker_.join();
}

std::vector<int> CostFieldUpdater::build(CostFieldKey key,
                                         const CostFieldSnapshot& snapshot) {
    const int cells = snapshot.width * snapshot.height;
    if (key.first < 0 || key.first >= cells) {
        throw std::invalid_argument("cost field target is outside chip");
    }
    std::vector<int> field(static_cast<std::size_t>(cells), unreachable);
    auto traversable = [&](int cell) {
        const int region = snapshot.upcoming_region[cell];
        return !snapshot.blocked[cell] &&
               (key.second == -1 || region == 0 || region == key.second);
    };
    std::queue<int> frontier;
    if (traversable(key.first)) {
        field[key.first] = 0;
        frontier.push(key.first);
    }
    while (!frontier.empty()) {
        const int cell = frontier.front();
        frontier.pop();
        const int x = cell % snapshot.width;
        const int y = cell / snapshot.width;
        const int adjacent[4] = {
            x + 1 < snapshot.width ? cell + 1 : -1,
            y + 1 < snapshot.height ? cell + snapshot.width : -1,
            x > 0 ? cell - 1 : -1,
            y > 0 ? cell - snapshot.width : -1};
        for (const int next : adjacent) {
            if (next < 0 || !traversable(next) || field[next] != unreachable) continue;
            field[next] = field[cell] + 1;
            frontier.push(next);
        }
    }
    return field;
}

void CostFieldUpdater::schedule(
    CostFieldKey key, std::shared_ptr<const CostFieldSnapshot> snapshot) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto latest = latest_version_.find(key);
    if (latest != latest_version_.end() && latest->second == snapshot->version) return;
    latest_version_[key] = snapshot->version;
    completed_.erase(key);
    if (pending_.count(key) == 0) order_.push_back(key);
    pending_[key] = Job{key, std::move(snapshot)};
    work_ready_.notify_one();
}

std::optional<std::vector<int>> CostFieldUpdater::take(
    CostFieldKey key, std::uint64_t version) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = completed_.find(key);
    if (found == completed_.end() || found->second.version != version) return std::nullopt;
    auto field = std::move(found->second.field);
    completed_.erase(found);
    return field;
}

std::optional<std::vector<int>> CostFieldUpdater::wait(
    CostFieldKey key, std::uint64_t version) {
    std::unique_lock<std::mutex> lock(mutex_);
    result_ready_.wait(lock, [&] {
        const auto latest = latest_version_.find(key);
        return stopping_ || latest == latest_version_.end() ||
               latest->second != version ||
               (completed_.count(key) != 0 && completed_.at(key).version == version);
    });
    const auto found = completed_.find(key);
    if (found == completed_.end() || found->second.version != version) return std::nullopt;
    auto field = std::move(found->second.field);
    completed_.erase(found);
    return field;
}

void CostFieldUpdater::cancel(CostFieldKey key) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.erase(key);
    order_.erase(std::remove(order_.begin(), order_.end(), key), order_.end());
    completed_.erase(key);
    latest_version_.erase(key);
    result_ready_.notify_all();
}

void CostFieldUpdater::run() {
    while (true) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            work_ready_.wait(lock, [&] { return stopping_ || !order_.empty(); });
            if (stopping_) return;
            const auto key = order_.front();
            order_.pop_front();
            const auto found = pending_.find(key);
            if (found == pending_.end()) continue;
            job = std::move(found->second);
            pending_.erase(found);
        }
        auto field = build(job.key, *job.snapshot);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto latest = latest_version_.find(job.key);
            if (latest != latest_version_.end() &&
                latest->second == job.snapshot->version && !stopping_) {
                completed_[job.key] = Result{job.snapshot->version, std::move(field)};
            }
        }
        result_ready_.notify_all();
    }
}

}  // namespace dmf
