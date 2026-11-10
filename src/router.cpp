#include "dmf/router.hpp"

#include <algorithm>
#include <limits>
#include <queue>
#include <set>
#include <stdexcept>
#include <utility>

namespace dmf {

namespace {
constexpr int unreachable = std::numeric_limits<int>::max();
}

Router::Router(int width, int height, DropletPositions& positions,
               int minimum_separation)
    : width_(width), height_(height), minimum_separation_(minimum_separation),
      positions_(positions), previous_positions_(positions) {
    if (width <= 0 || height <= 0 || minimum_separation < 1) {
        throw std::invalid_argument("invalid router dimensions or separation");
    }
    blocked_.assign(static_cast<std::size_t>(width_) * height_, 0);
    upcoming_region_.assign(blocked_.size(), 0);
    auto occupied = occupancy();
    for (const auto& [droplet, point] : positions_) {
        if (droplet <= 0 || !contains(point) || !legal(point, droplet, occupied)) {
            throw std::invalid_argument("invalid initial droplet positions");
        }
    }
}

bool Router::contains(Point point) const {
    return point.x >= 0 && point.x < width_ && point.y >= 0 && point.y < height_;
}

int Router::index(Point point) const {
    return point.y * width_ + point.x;
}

Point Router::pointAt(int cell) const {
    return {cell % width_, cell / width_};
}

std::array<Point, 4> Router::neighbors(Point point) const {
    return {{{point.x + 1, point.y}, {point.x, point.y + 1},
             {point.x - 1, point.y}, {point.x, point.y - 1}}};
}

Router::Occupancy Router::occupancy() const {
    const auto cells = static_cast<std::size_t>(width_) * height_;
    Occupancy result{{std::vector<DropletId>(cells, 0)},
                     {std::vector<DropletId>(cells, 0)},
                     {std::vector<DropletId>(cells, 0)}};
    for (const auto& [droplet, point] : positions_) {
        if (!contains(point)) throw std::logic_error("droplet is outside chip");
        const auto cell = index(point);
        if (result.current[cell] != 0) throw std::logic_error("two droplets share an electrode");
        result.current[cell] = droplet;
        const auto previous = previous_positions_.find(droplet);
        const auto old = previous == previous_positions_.end() ? point : previous->second;
        if (contains(old)) result.previous[index(old)] = droplet;
    }
    return result;
}

bool Router::legal(Point candidate, DropletId droplet,
                   const Occupancy& occupied) const {
    if (!contains(candidate) || blocked_[index(candidate)]) return false;
    const int radius = minimum_separation_ - 1;
    for (int y = std::max(0, candidate.y - radius);
         y <= std::min(height_ - 1, candidate.y + radius); ++y) {
        for (int x = std::max(0, candidate.x - radius);
             x <= std::min(width_ - 1, candidate.x + radius); ++x) {
            const auto cell = index({x, y});
            if ((occupied.current[cell] != 0 && occupied.current[cell] != droplet) ||
                (occupied.previous[cell] != 0 && occupied.previous[cell] != droplet) ||
                (occupied.planned[cell] != 0 && occupied.planned[cell] != droplet)) {
                return false;
            }
        }
    }
    return true;
}

int Router::upcomingRegion(Point point) const {
    return contains(point) ? upcoming_region_[index(point)] : 0;
}

bool Router::upcomingBlocks(Point point, int allowed_module) const {
    if (allowed_module == -1) return false;
    const int region = upcomingRegion(point);
    return region != 0 && region != allowed_module;
}

bool Router::needsEvacuation(DropletId droplet, Point position) const {
    const int region = upcomingRegion(position);
    if (region == 0) return false;
    const auto owner = upcoming_input_owners_.find(droplet);
    return owner == upcoming_input_owners_.end() || owner->second != region;
}

std::optional<Point> Router::nearestExit(DropletId droplet,
                                         const Occupancy& occupied) const {
    const auto start = positions_.at(droplet);
    const int occupied_region = upcomingRegion(start);
    if (occupied_region == 0) return std::nullopt;

    std::vector<unsigned char> visited(blocked_.size(), 0);
    std::queue<Point> frontier;
    visited[index(start)] = 1;
    frontier.push(start);
    while (!frontier.empty()) {
        const auto point = frontier.front();
        frontier.pop();
        for (const auto next : neighbors(point)) {
            if (!contains(next) || visited[index(next)] ||
                !legal(next, droplet, occupied)) continue;
            const int region = upcomingRegion(next);
            if (region != 0 && region != occupied_region) continue;
            if (region == 0) return next;
            visited[index(next)] = 1;
            frontier.push(next);
        }
    }
    return std::nullopt;
}

void Router::refreshEvacuations(const Occupancy& occupied) {
    waiting_for_evacuation_.clear();
    for (auto it = evacuation_targets_.begin(); it != evacuation_targets_.end();) {
        const auto position = positions_.find(it->first);
        if (position == positions_.end() || !needsEvacuation(it->first, position->second)) {
            it = evacuation_targets_.erase(it);
        } else {
            ++it;
        }
    }
    for (const auto& [droplet, position] : positions_) {
        if (!needsEvacuation(droplet, position) ||
            evacuation_targets_.count(droplet) != 0) continue;
        const auto exit = nearestExit(droplet, occupied);
        if (exit) evacuation_targets_.emplace(droplet, *exit);
        else waiting_for_evacuation_.push_back(droplet);
    }
}

bool Router::hasTasks() const {
    if (!tasks_.empty() || !evacuation_targets_.empty()) return true;
    return std::any_of(positions_.begin(), positions_.end(), [&](const auto& entry) {
        return needsEvacuation(entry.first, entry.second);
    });
}

int Router::futureCost(Point candidate, DropletId droplet,
                       const Occupancy& occupied, const std::vector<int>& field) const {
    int best = field[index(candidate)];
    for (const auto next : neighbors(candidate)) {
        if (legal(next, droplet, occupied)) best = std::min(best, field[index(next)]);
    }
    return best;
}

const std::vector<int>& Router::fieldFor(Point target, int allowed_module) {
    const auto target_cell = index(target);
    const auto key = std::make_pair(target_cell, allowed_module);
    const auto found = fields_.find(key);
    if (found != fields_.end()) return found->second;

    std::vector<int> field(blocked_.size(), unreachable);
    std::queue<int> frontier;
    if (!blocked_[target_cell] && !upcomingBlocks(target, allowed_module)) {
        field[target_cell] = 0;
        frontier.push(target_cell);
    }
    while (!frontier.empty()) {
        const int cell = frontier.front();
        frontier.pop();
        for (const auto next : neighbors(pointAt(cell))) {
            if (!contains(next)) continue;
            const int next_cell = index(next);
            if (blocked_[next_cell] || upcomingBlocks(next, allowed_module) ||
                field[next_cell] != unreachable) continue;
            field[next_cell] = field[cell] + 1;
            frontier.push(next_cell);
        }
    }
    ++field_builds_;
    return fields_.emplace(key, std::move(field)).first->second;
}

std::vector<RouteArrival> Router::addTasks(const std::vector<RoutingTask>& tasks) {
    if (!pending_.empty()) throw std::logic_error("movement feedback is still pending");
    std::set<DropletId> new_droplets;
    for (const auto& task : tasks) {
        if (task.droplet <= 0 || task.operation <= 0 || !contains(task.target) ||
            positions_.count(task.droplet) == 0 || tasks_.count(task.droplet) != 0 ||
            !new_droplets.insert(task.droplet).second) {
            throw std::invalid_argument("invalid or duplicate routing task");
        }
    }
    std::vector<RouteArrival> arrived;
    for (const auto& task : tasks) {
        if (positions_.at(task.droplet) == task.target) {
            arrived.push_back({task.operation, task.droplet});
        } else {
            tasks_.emplace(task.droplet, task);
        }
    }
    return arrived;
}

void Router::setBlockages(const std::vector<VirtualModule>& modules) {
    if (!pending_.empty()) throw std::logic_error("movement feedback is still pending");
    std::vector<std::tuple<int, int, int, int>> signature;
    for (const auto& module : modules) {
        if (module.side <= 0 || !contains(module.top_left) ||
            !contains({module.top_left.x + module.side - 1,
                       module.top_left.y + module.side - 1})) {
            throw std::invalid_argument("blockage is outside chip");
        }
        signature.emplace_back(module.id, module.top_left.x,
                               module.top_left.y, module.side);
    }
    std::sort(signature.begin(), signature.end());
    if (signature == blockage_signature_) return;
    std::vector<unsigned char> next(blocked_.size(), 0);
    for (const auto& module : modules) {
        for (int y = module.top_left.y; y < module.top_left.y + module.side; ++y) {
            for (int x = module.top_left.x; x < module.top_left.x + module.side; ++x) {
                next[index({x, y})] = 1;
            }
        }
    }
    for (const auto& [droplet, task] : tasks_) {
        (void)task;
        if (next[index(positions_.at(droplet))]) {
            throw std::logic_error("new blockage traps a routing droplet");
        }
    }
    blockage_signature_ = std::move(signature);
    blocked_ = std::move(next);
    fields_.clear();
    evacuation_targets_.clear();
}

void Router::setUpcomingBlockages(const std::vector<UpcomingBlockage>& modules) {
    if (!pending_.empty()) throw std::logic_error("movement feedback is still pending");
    std::vector<std::tuple<int, int, int, int, std::vector<DropletId>>> signature;
    std::map<DropletId, int> owners;
    std::set<int> module_ids;
    for (const auto& upcoming : modules) {
        const auto& module = upcoming.module;
        if (module.id <= 0 || module.side <= 0 || !contains(module.top_left) ||
            !contains({module.top_left.x + module.side - 1,
                       module.top_left.y + module.side - 1}) ||
            !module_ids.insert(module.id).second) {
            throw std::invalid_argument("invalid upcoming blockage");
        }
        auto inputs = upcoming.inputs;
        std::sort(inputs.begin(), inputs.end());
        if (std::adjacent_find(inputs.begin(), inputs.end()) != inputs.end()) {
            throw std::invalid_argument("duplicate upcoming input droplet");
        }
        for (const auto droplet : inputs) {
            if (droplet <= 0 || !owners.emplace(droplet, module.id).second) {
                throw std::invalid_argument("invalid upcoming input droplet");
            }
        }
        signature.emplace_back(module.id, module.top_left.x,
                               module.top_left.y, module.side, std::move(inputs));
    }
    std::sort(signature.begin(), signature.end());
    if (signature == upcoming_signature_) return;
    std::vector<int> next_region(blocked_.size(), 0);
    for (const auto& upcoming : modules) {
        const auto& module = upcoming.module;
        for (int y = module.top_left.y; y < module.top_left.y + module.side; ++y) {
            for (int x = module.top_left.x; x < module.top_left.x + module.side; ++x) {
                const auto cell = index({x, y});
                if (next_region[cell] != 0) {
                    throw std::invalid_argument("upcoming blockages overlap");
                }
                next_region[cell] = module.id;
            }
        }
    }
    upcoming_signature_ = std::move(signature);
    upcoming_region_ = std::move(next_region);
    upcoming_input_owners_ = std::move(owners);
    evacuation_targets_.clear();
    waiting_for_evacuation_.clear();
    fields_.clear();
}

std::vector<MoveCommand> Router::planStep() {
    if (!pending_.empty()) throw std::logic_error("movement feedback is still pending");
    auto occupied = occupancy();
    refreshEvacuations(occupied);
    evacuated_last_step_.clear();
    std::set<DropletId> moving;
    for (const auto& [droplet, task] : tasks_) {
        (void)task;
        moving.insert(droplet);
    }
    for (const auto& [droplet, target] : evacuation_targets_) {
        (void)target;
        moving.insert(droplet);
    }
    for (const auto droplet : moving) {
        const auto current = positions_.at(droplet);
        const auto evacuation = evacuation_targets_.find(droplet);
        if (evacuation == evacuation_targets_.end() &&
            needsEvacuation(droplet, current)) continue;
        const bool evacuating = evacuation != evacuation_targets_.end();
        const auto target = evacuating ? evacuation->second : tasks_.at(droplet).target;
        const auto owner = upcoming_input_owners_.find(droplet);
        int allowed_module = 0;
        if (evacuating) allowed_module = -1;
        else if (owner != upcoming_input_owners_.end()) allowed_module = owner->second;
        const int occupied_region = evacuating ? upcomingRegion(current) : 0;
        const auto& field = fieldFor(target, allowed_module);
        Point chosen = current;
        int best_cost = unreachable;
        int best_future = unreachable;
        bool best_backtracks = true;
        auto candidates = neighbors(current);
        if (droplet % 2 == 0) std::swap(candidates[1], candidates[3]);
        for (const auto candidate : candidates) {
            if (!legal(candidate, droplet, occupied)) continue;
            if (evacuating) {
                const int region = upcomingRegion(candidate);
                if (region != 0 && region != occupied_region) continue;
            } else if (upcomingBlocks(candidate, allowed_module)) {
                continue;
            }
            const int cost = field[index(candidate)];
            if (cost == unreachable) continue;
            const int future = futureCost(candidate, droplet, occupied, field);
            const auto previous = previous_positions_.find(droplet);
            const bool backtracks = previous != previous_positions_.end() &&
                                    candidate == previous->second;
            const int adjusted_cost = cost + (backtracks ? 3 : 0);
            if (adjusted_cost < best_cost ||
                (adjusted_cost == best_cost && future < best_future) ||
                (adjusted_cost == best_cost && future == best_future &&
                 best_backtracks && !backtracks)) {
                chosen = candidate;
                best_cost = adjusted_cost;
                best_future = future;
                best_backtracks = backtracks;
            }
        }
        if (!legal(chosen, droplet, occupied)) {
            throw std::logic_error("no safe position for routing droplet");
        }
        if (evacuating && chosen == current &&
            std::find(waiting_for_evacuation_.begin(),
                      waiting_for_evacuation_.end(), droplet) ==
                waiting_for_evacuation_.end()) {
            waiting_for_evacuation_.push_back(droplet);
        }
        occupied.planned[index(chosen)] = droplet;
        pending_.push_back({droplet, current, chosen});
    }
    return pending_;
}

void Router::discardUnusedFields(Point target) {
    const int cell = index(target);
    const bool still_used = std::any_of(tasks_.begin(), tasks_.end(), [&](const auto& entry) {
        return index(entry.second.target) == cell;
    }) || std::any_of(evacuation_targets_.begin(), evacuation_targets_.end(),
                     [&](const auto& entry) { return index(entry.second) == cell; });
    if (still_used) return;
    for (auto it = fields_.begin(); it != fields_.end();) {
        if (it->first.first == cell) it = fields_.erase(it);
        else ++it;
    }
}

std::vector<RouteArrival> Router::applyFeedback(
    const std::vector<MoveFeedback>& feedback) {
    if (feedback.size() != pending_.size()) {
        throw std::invalid_argument("feedback must cover every planned droplet");
    }
    std::map<DropletId, Point> observed;
    for (const auto& entry : feedback) {
        if (!observed.emplace(entry.droplet, entry.observed).second) {
            throw std::invalid_argument("duplicate movement feedback");
        }
    }
    auto occupied = occupancy();
    for (const auto& command : pending_) {
        const auto found = observed.find(command.droplet);
        if (found == observed.end() ||
            (!(found->second == command.from) && !(found->second == command.to))) {
            throw std::invalid_argument("observed position does not match planned move or failure");
        }
        occupied.planned[index(found->second)] = command.droplet;
    }
    for (const auto& command : pending_) {
        if (!legal(observed.at(command.droplet), command.droplet, occupied)) {
            throw std::invalid_argument("movement feedback violates fluidic separation");
        }
    }

    previous_positions_ = positions_;
    std::vector<RouteArrival> arrived;
    for (const auto& command : pending_) {
        const auto position = observed.at(command.droplet);
        positions_[command.droplet] = position;
        const auto evacuation = evacuation_targets_.find(command.droplet);
        if (evacuation != evacuation_targets_.end() &&
            position == evacuation->second) {
            const auto target = evacuation->second;
            evacuation_targets_.erase(evacuation);
            evacuated_last_step_.push_back(command.droplet);
            discardUnusedFields(target);
        }
        const auto task = tasks_.find(command.droplet);
        if (task != tasks_.end() && position == task->second.target) {
            const auto target = task->second.target;
            arrived.push_back({task->second.operation, command.droplet});
            tasks_.erase(task);
            discardUnusedFields(target);
        }
    }
    pending_.clear();
    return arrived;
}

}  // namespace dmf
