#include "dmf/router.hpp"

#include <algorithm>
#include <cstdlib>
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
    field_snapshot_ = std::make_shared<CostFieldSnapshot>(
        CostFieldSnapshot{width_, height_, 0, blocked_, upcoming_region_});
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
    if (crossing_modules_.count(droplet) != 0) return false;
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
    const CostFieldKey key{index(target), allowed_module};
    const auto found = fields_.find(key);
    if (found != fields_.end()) {
        if (field_versions_.at(key) != field_snapshot_->version) {
            auto updated = field_updater_.take(key, field_snapshot_->version);
            if (updated) installUpdatedField(key, std::move(*updated));
            else ++stale_field_decisions_;
        }
        return found->second;
    }

    auto field = CostFieldUpdater::build(key, *field_snapshot_);
    ++field_builds_;
    field_versions_[key] = field_snapshot_->version;
    return fields_.emplace(key, std::move(field)).first->second;
}

void Router::installUpdatedField(CostFieldKey key, std::vector<int> field) {
    fields_.at(key) = std::move(field);
    field_versions_.at(key) = field_snapshot_->version;
    ++field_builds_;
    ++async_field_swaps_;
}

void Router::environmentChanged() {
    open_components_.clear();
    future_components_.clear();
    crossing_paths_.clear();
    field_snapshot_ = std::make_shared<CostFieldSnapshot>(CostFieldSnapshot{
        width_, height_, field_snapshot_->version + 1, blocked_, upcoming_region_});
    for (const auto& [key, field] : fields_) {
        (void)field;
        field_updater_.schedule(key, field_snapshot_);
    }
}

std::vector<int> Router::components(
    const std::vector<unsigned char>& blocked) const {
    std::vector<int> labels(blocked.size(), 0);
    std::queue<int> frontier;
    int component = 0;
    for (int start = 0; start < static_cast<int>(blocked.size()); ++start) {
        if (blocked[start] || labels[start] != 0) continue;
        labels[start] = ++component;
        frontier.push(start);
        while (!frontier.empty()) {
            const int cell = frontier.front();
            frontier.pop();
            const Point point{cell % width_, cell / width_};
            for (const auto next : neighbors(point)) {
                if (!contains(next)) continue;
                const int next_cell = index(next);
                if (blocked[next_cell] || labels[next_cell] != 0) continue;
                labels[next_cell] = component;
                frontier.push(next_cell);
            }
        }
    }
    return labels;
}

const std::vector<int>& Router::openComponents() {
    if (open_components_.empty()) open_components_ = components(blocked_);
    return open_components_;
}

const std::vector<int>& Router::futureComponents(const VirtualModule& module) {
    const auto found = future_components_.find(module.id);
    if (found != future_components_.end()) return found->second;
    auto blocked = blocked_;
    for (int y = module.top_left.y; y < module.top_left.y + module.side; ++y) {
        for (int x = module.top_left.x; x < module.top_left.x + module.side; ++x) {
            blocked[index({x, y})] = 1;
        }
    }
    return future_components_.emplace(module.id, components(blocked))
        .first->second;
}

void Router::prepareUpcomingCrossings() {
    const auto previous_crossings = crossing_modules_;
    crossing_modules_.clear();
    activation_holds_.clear();
    if (upcoming_blockages_.empty()) return;
    for (const auto& [droplet, task] : tasks_) {
        const int current = index(positions_.at(droplet));
        const auto& open = openComponents();
        if (open[current] == 0 || open[current] != open[index(task.target)]) continue;
        for (const auto& upcoming : upcoming_blockages_) {
            const auto& module = upcoming.module;
            if (task.module_id == module.id) continue;
            const auto position = positions_.at(droplet);
            const bool inside = position.x >= module.top_left.x &&
                                position.x < module.top_left.x + module.side &&
                                position.y >= module.top_left.y &&
                                position.y < module.top_left.y + module.side;
            const auto previous = previous_crossings.find(droplet);
            if (inside && (previous == previous_crossings.end() ||
                           previous->second != module.id)) continue;
            const auto& future = futureComponents(module);
            if (future[current] != 0 &&
                future[current] == future[index(task.target)]) continue;
            crossing_modules_.emplace(droplet, module.id);
            break;
        }
    }
    for (auto it = crossing_paths_.begin(); it != crossing_paths_.end();) {
        const auto crossing = crossing_modules_.find(it->first);
        if (crossing == crossing_modules_.end() ||
            crossing->second != it->second.first) {
            it = crossing_paths_.erase(it);
        } else {
            ++it;
        }
    }
    if (!crossing_modules_.empty()) {
        // A crossing may use any reserved area, so none can activate mid-crossing.
        for (const auto& upcoming : upcoming_blockages_) {
            activation_holds_.insert(upcoming.module.id);
        }
    }
}

std::optional<Point> Router::nextCrossingMove(DropletId droplet, int module_id,
                                               const Occupancy& occupied) {
    const auto start = positions_.at(droplet);
    auto cached = crossing_paths_.find(droplet);
    if (cached != crossing_paths_.end() && !cached->second.second.empty()) {
        const auto next = cached->second.second.front();
        const int distance = std::abs(next.x - start.x) + std::abs(next.y - start.y);
        if (cached->second.first == module_id && distance == 1 &&
            legal(next, droplet, occupied)) return next;
    }
    crossing_paths_.erase(droplet);

    const auto module = std::find_if(upcoming_blockages_.begin(),
                                     upcoming_blockages_.end(),
                                     [&](const auto& entry) {
                                         return entry.module.id == module_id;
                                     });
    if (module == upcoming_blockages_.end()) return std::nullopt;
    const auto& components = futureComponents(module->module);
    const int target_side = components[index(tasks_.at(droplet).target)];
    if (target_side == 0) return std::nullopt;
    std::vector<int> previous(blocked_.size(), -1);
    std::queue<int> frontier;
    const int start_cell = index(start);
    previous[start_cell] = start_cell;
    frontier.push(start_cell);
    int exit_cell = -1;
    while (!frontier.empty() && exit_cell == -1) {
        const int cell = frontier.front();
        frontier.pop();
        const Point point{cell % width_, cell / width_};
        for (const auto next : neighbors(point)) {
            if (!legal(next, droplet, occupied)) continue;
            const int next_cell = index(next);
            if (previous[next_cell] != -1) continue;
            previous[next_cell] = cell;
            if (components[next_cell] == target_side) {
                exit_cell = next_cell;
                break;
            }
            frontier.push(next_cell);
        }
    }
    if (exit_cell == -1) return std::nullopt;
    std::deque<Point> path;
    for (int cell = exit_cell; cell != start_cell; cell = previous[cell]) {
        path.push_front({cell % width_, cell / width_});
    }
    const Point next = path.front();
    crossing_paths_[droplet] = {module_id, std::move(path)};
    return next;
}

void Router::waitForCostFieldUpdates() {
    for (const auto& [key, field] : fields_) {
        (void)field;
        if (field_versions_.at(key) == field_snapshot_->version) continue;
        auto updated = field_updater_.wait(key, field_snapshot_->version);
        if (updated) installUpdatedField(key, std::move(*updated));
    }
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
    environmentChanged();
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
    upcoming_blockages_ = modules;
    evacuation_targets_.clear();
    waiting_for_evacuation_.clear();
    environmentChanged();
}

std::vector<MoveCommand> Router::planStep() {
    if (!pending_.empty()) throw std::logic_error("movement feedback is still pending");
    prepareUpcomingCrossings();
    waiting_for_crossing_.clear();
    auto occupied = occupancy();
    refreshEvacuations(occupied);
    evacuated_last_step_.clear();
    std::set<DropletId> moving;
    std::set<int> modules_with_crossings;
    for (const auto& [droplet, module_id] : crossing_modules_) {
        (void)droplet;
        modules_with_crossings.insert(module_id);
    }
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
        const auto crossing = crossing_modules_.find(droplet);
        const auto evacuation = evacuation_targets_.find(droplet);
        if (evacuation == evacuation_targets_.end() &&
            needsEvacuation(droplet, current)) continue;
        const bool evacuating = evacuation != evacuation_targets_.end();
        const auto target = evacuating ? evacuation->second : tasks_.at(droplet).target;
        if (!evacuating && crossing == crossing_modules_.end() &&
            modules_with_crossings.count(tasks_.at(droplet).module_id) != 0) {
            occupied.planned[index(current)] = droplet;
            pending_.push_back({droplet, current, current});
            continue;
        }
        if (crossing != crossing_modules_.end()) {
            const auto next = nextCrossingMove(droplet, crossing->second, occupied);
            if (!next) waiting_for_crossing_.push_back(droplet);
            const auto chosen = next.value_or(current);
            if (!legal(chosen, droplet, occupied)) {
                throw std::logic_error("no safe position for crossing droplet");
            }
            occupied.planned[index(chosen)] = droplet;
            pending_.push_back({droplet, current, chosen});
            continue;
        }
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
        if (it->first.first == cell) {
            field_versions_.erase(it->first);
            field_updater_.cancel(it->first);
            it = fields_.erase(it);
        } else ++it;
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
        const auto path = crossing_paths_.find(command.droplet);
        if (path != crossing_paths_.end() && !path->second.second.empty() &&
            position == path->second.second.front()) {
            path->second.second.pop_front();
        }
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
