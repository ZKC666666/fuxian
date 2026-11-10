#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace dmf {

using CostFieldKey = std::pair<int, int>;  // Target cell and allowed upcoming module.

struct CostFieldSnapshot {
    int width;
    int height;
    std::uint64_t version;
    std::vector<unsigned char> blocked;
    std::vector<int> upcoming_region;
};

// Owns one worker. Jobs use immutable snapshots; only completed fields cross threads.
class CostFieldUpdater {
public:
    CostFieldUpdater();
    ~CostFieldUpdater();
    CostFieldUpdater(const CostFieldUpdater&) = delete;
    CostFieldUpdater& operator=(const CostFieldUpdater&) = delete;

    static std::vector<int> build(CostFieldKey key, const CostFieldSnapshot& snapshot);
    void schedule(CostFieldKey key, std::shared_ptr<const CostFieldSnapshot> snapshot);
    std::optional<std::vector<int>> take(CostFieldKey key, std::uint64_t version);
    std::optional<std::vector<int>> wait(CostFieldKey key, std::uint64_t version);
    void cancel(CostFieldKey key);

private:
    struct Job {
        CostFieldKey key;
        std::shared_ptr<const CostFieldSnapshot> snapshot;
    };
    struct Result {
        std::uint64_t version;
        std::vector<int> field;
    };

    std::mutex mutex_;
    std::condition_variable work_ready_;
    std::condition_variable result_ready_;
    std::deque<CostFieldKey> order_;
    std::map<CostFieldKey, Job> pending_;
    std::map<CostFieldKey, Result> completed_;
    std::map<CostFieldKey, std::uint64_t> latest_version_;
    std::thread worker_;
    bool stopping_ = false;

    void run();
};

}  // namespace dmf
