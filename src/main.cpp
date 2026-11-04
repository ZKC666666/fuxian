#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <queue>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace dmf {

struct Point {
    int x = 0;
    int y = 0;
};
bool operator==(Point a, Point b) { return a.x == b.x && a.y == b.y; }
bool operator!=(Point a, Point b) { return !(a == b); }
int chebyshev(Point a, Point b) { return std::max(std::abs(a.x - b.x), std::abs(a.y - b.y)); }

struct Rect {
    int x = 0;
    int y = 0;
    int width = 3;
    int height = 3;
    bool contains(Point p) const {
        return p.x >= x && p.x < x + width && p.y >= y && p.y < y + height;
    }
    Point center() const { return {x + width / 2, y + height / 2}; }
};

class Router {
public:
    struct Droplet {
        int id;
        Point position;
        std::optional<Point> target;
    };

    Router(int width, int height, int separation = 2, std::uint32_t seed = 1)
        : width_(width), height_(height), separation_(separation),
          blocks_(static_cast<std::size_t>(width * height), 0), rng_(seed) {
        if (width <= 0 || height <= 0 || separation < 1) throw std::invalid_argument("invalid grid");
    }

    bool inside(Point p) const { return p.x >= 0 && p.x < width_ && p.y >= 0 && p.y < height_; }
    bool blocked(Point p) const { return !inside(p) || blocks_[index(p)] != 0; }
    bool present(int id) const { return droplets_.count(id) != 0; }
    Point position(int id) const { return droplets_.at(id).position; }
    bool arrived(int id) const {
        const auto& d = droplets_.at(id);
        return d.target && d.position == *d.target;
    }
    std::size_t dropletCount() const { return droplets_.size(); }
    std::size_t completedRoutes() const { return completed_routes_; }
    double decisionMilliseconds() const { return decision_milliseconds_; }
    bool moduleClearExcept(Rect r, const std::vector<int>& allowed) const {
        for (const auto& [id, d] : droplets_) {
            if (r.contains(d.position) &&
                std::find(allowed.begin(), allowed.end(), id) == allowed.end()) return false;
        }
        return true;
    }

    int addDroplet(Point p) {
        if (blocked(p)) throw std::runtime_error("droplet starts in blocked cell");
        for (const auto& [id, other] : droplets_) {
            (void)id;
            if (chebyshev(p, other.position) < separation_) {
                throw std::runtime_error("initial droplets violate separation");
            }
        }
        int id = next_id_++;
        droplets_.emplace(id, Droplet{id, p, std::nullopt});
        return id;
    }

    void removeDroplet(int id) { droplets_.erase(id); }
    void setTarget(int id, Point p) {
        if (blocked(p)) throw std::runtime_error("target is blocked");
        droplets_.at(id).target = p;
        if (droplets_.at(id).position == p) ++completed_routes_;
    }

    void changeBlock(Rect r, bool enabled) {
        for (int y = r.y; y < r.y + r.height; ++y) {
            for (int x = r.x; x < r.x + r.width; ++x) {
                Point p{x, y};
                if (!inside(p)) throw std::runtime_error("blockage outside chip");
                if (enabled) {
                    for (const auto& [id, d] : droplets_) {
                        (void)id;
                        if (d.position == p) throw std::runtime_error("blockage covers droplet");
                    }
                }
            }
        }
        for (int y = r.y; y < r.y + r.height; ++y) {
            for (int x = r.x; x < r.x + r.width; ++x) {
                auto& cell = blocks_[index({x, y})];
                if (enabled) ++cell;
                else {
                    if (cell == 0) throw std::runtime_error("clearing absent blockage");
                    --cell;
                }
            }
        }
        ++block_version_;
    }

    // One synchronized electrode-control step. Inactive droplets remain physical obstacles.
    void step(double movement_failure_probability = 0.0) {
        if (movement_failure_probability < 0 || movement_failure_probability > 1)
            throw std::invalid_argument("invalid failure probability");
        auto start = std::chrono::steady_clock::now();
        std::map<int, Point> next;
        std::vector<int> order;
        for (const auto& [id, d] : droplets_) if (d.target && d.position != *d.target) order.push_back(id);
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            const auto& da = droplets_.at(a);
            const auto& db = droplets_.at(b);
            int ca = field(*da.target)[index(da.position)];
            int cb = field(*db.target)[index(db.position)];
            return ca == cb ? a < b : ca > cb;
        });

        for (int id : order) {
            const auto& d = droplets_.at(id);
            const auto& cost = field(*d.target);
            std::vector<Point> choices = {d.position, {d.position.x + 1, d.position.y},
                                          {d.position.x - 1, d.position.y}, {d.position.x, d.position.y + 1},
                                          {d.position.x, d.position.y - 1}};
            std::sort(choices.begin(), choices.end(), [&](Point a, Point b) {
                int ac = inside(a) ? cost[index(a)] : infinity;
                int bc = inside(b) ? cost[index(b)] : infinity;
                if (ac != bc) return ac < bc;
                int a2 = lookahead(cost, a), b2 = lookahead(cost, b);
                if (a2 != b2) return a2 < b2;
                if (a == d.position || b == d.position) return b == d.position;
                return a.y == b.y ? a.x < b.x : a.y < b.y;
            });
            Point selected = d.position;
            for (Point candidate : choices) {
                if (blocked(candidate) || !safeAgainstOthers(id, candidate, next)) continue;
                selected = candidate;
                break;
            }
            next[id] = selected;
        }

        // A failed move stays in place. Reject the entire step if feedback makes another
        // planned move unsafe; this models a conservative control response.
        std::bernoulli_distribution fails(movement_failure_probability);
        for (auto& [id, p] : next) {
            if (p != droplets_.at(id).position && fails(rng_)) p = droplets_.at(id).position;
        }
        if (!safeConfiguration(next)) {
            for (auto& [id, p] : next) p = droplets_.at(id).position;
        }
        for (const auto& [id, p] : next) {
            auto& d = droplets_.at(id);
            bool newly_arrived = d.target && d.position != *d.target && p == *d.target;
            d.position = p;
            if (newly_arrived) ++completed_routes_;
        }
        decision_milliseconds_ +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

    bool separationValid() const {
        for (auto a = droplets_.begin(); a != droplets_.end(); ++a) {
            for (auto b = std::next(a); b != droplets_.end(); ++b) {
                if (chebyshev(a->second.position, b->second.position) < separation_) return false;
            }
        }
        return true;
    }

private:
    static constexpr int infinity = std::numeric_limits<int>::max() / 4;
    struct CachedField { std::uint64_t version = 0; std::vector<int> costs; };
    int width_, height_, separation_, next_id_ = 1;
    std::vector<int> blocks_;
    std::map<int, Droplet> droplets_;
    std::map<std::pair<int, int>, CachedField> fields_;
    std::uint64_t block_version_ = 1;
    std::mt19937 rng_;
    std::size_t completed_routes_ = 0;
    double decision_milliseconds_ = 0;

    std::size_t index(Point p) const { return static_cast<std::size_t>(p.y * width_ + p.x); }

    const std::vector<int>& field(Point goal) {
        auto& cache = fields_[{goal.x, goal.y}];
        if (cache.version == block_version_) return cache.costs;
        cache.costs.assign(blocks_.size(), infinity);
        if (!blocked(goal)) {
            std::queue<Point> q;
            cache.costs[index(goal)] = 0;
            q.push(goal);
            while (!q.empty()) {
                Point p = q.front(); q.pop();
                for (Point n : {Point{p.x + 1, p.y}, Point{p.x - 1, p.y},
                                Point{p.x, p.y + 1}, Point{p.x, p.y - 1}}) {
                    if (!blocked(n) && cache.costs[index(n)] == infinity) {
                        cache.costs[index(n)] = cache.costs[index(p)] + 1;
                        q.push(n);
                    }
                }
            }
        }
        cache.version = block_version_;
        return cache.costs;
    }

    int lookahead(const std::vector<int>& cost, Point p) const {
        if (blocked(p)) return infinity;
        int best = cost[index(p)];
        for (Point n : {Point{p.x + 1, p.y}, Point{p.x - 1, p.y},
                        Point{p.x, p.y + 1}, Point{p.x, p.y - 1}}) {
            if (!blocked(n)) best = std::min(best, cost[index(n)]);
        }
        return best;
    }

    bool safeAgainstOthers(int id, Point p, const std::map<int, Point>& planned) const {
        for (const auto& [other_id, other] : droplets_) {
            if (other_id == id) continue;
            if (chebyshev(p, other.position) < separation_) return false;
            auto it = planned.find(other_id);
            if (it != planned.end() && chebyshev(p, it->second) < separation_) return false;
        }
        return true;
    }

    bool safeConfiguration(const std::map<int, Point>& planned) const {
        for (auto a = droplets_.begin(); a != droplets_.end(); ++a) {
            Point pa = planned.count(a->first) ? planned.at(a->first) : a->second.position;
            if (blocked(pa)) return false;
            for (auto b = std::next(a); b != droplets_.end(); ++b) {
                Point pb = planned.count(b->first) ? planned.at(b->first) : b->second.position;
                if (chebyshev(pa, pb) < separation_) return false;
            }
        }
        return true;
    }
};

enum class OperationState { Pending, Placed, Active, Done };
enum class OperationType { Mix, Detect, Output, Waste };

struct Input {
    int source_operation = 0;
    int initial_droplet = 0;
};

struct Operation {
    int id = 0;
    OperationType type = OperationType::Mix;
    std::vector<Input> inputs;
    int duration = 1;
    OperationState state = OperationState::Pending;
    Rect module;
    std::vector<int> input_droplets;
    int output_droplet = 0;
    int remaining = 0;
};

class Simulator {
public:
    Simulator(int width, int height) : router_(width, height), width_(width), height_(height) {}
    Router& router() { return router_; }
    const Router& router() const { return router_; }
    int cycles() const { return cycle_; }
    int completedOperations() const { return completed_operations_; }
    int activatedBlocks() const { return activated_blocks_; }

    void appendBlock(std::vector<Operation> block) {
        for (auto& op : block) {
            if (operations_.count(op.id)) throw std::runtime_error("duplicate operation id");
            operations_.emplace(op.id, std::move(op));
        }
        ++activated_blocks_;
    }

    // Demo branch feedback is injected on completion of detection operations.
    void setDetectionBranch(int detection_id, int branch_id, OperationType type) {
        branches_[detection_id] = {branch_id, type};
    }

    bool tick() {
        placeReady();
        startArrived();
        router_.step();
        finishOperations();
        ++cycle_;
        return finished();
    }

    bool finished() const {
        if (!branches_.empty()) return false;
        return std::all_of(operations_.begin(), operations_.end(), [](const auto& item) {
            return item.second.state == OperationState::Done;
        });
    }

private:
    Router router_;
    int width_, height_, cycle_ = 0, activated_blocks_ = 0, completed_operations_ = 0;
    std::map<int, Operation> operations_;
    std::map<int, std::pair<int, OperationType>> branches_;

    std::optional<std::vector<int>> readyInputs(const Operation& op) const {
        std::vector<int> ids;
        for (const auto& input : op.inputs) {
            int id = input.initial_droplet;
            if (input.source_operation != 0) {
                auto it = operations_.find(input.source_operation);
                if (it == operations_.end() || it->second.state != OperationState::Done) return std::nullopt;
                id = it->second.output_droplet;
            }
            if (id == 0 || !router_.present(id)) return std::nullopt;
            ids.push_back(id);
        }
        return ids;
    }

    static bool closeModules(Rect a, Rect b) {
        return a.x < b.x + b.width + 3 && b.x < a.x + a.width + 3 &&
               a.y < b.y + b.height + 3 && b.y < a.y + a.height + 3;
    }

    std::optional<Rect> findPlacement(const std::vector<int>& input_ids) const {
        double cx = 0, cy = 0;
        for (int id : input_ids) {
            auto p = router_.position(id);
            cx += p.x; cy += p.y;
        }
        cx /= input_ids.size(); cy /= input_ids.size();
        std::optional<Rect> best;
        double best_score = std::numeric_limits<double>::infinity();
        for (int y = 1; y + 2 < height_; y += 6) {
            for (int x = 1; x + 2 < width_; x += 6) {
                Rect r{x, y, 3, 3};
                bool occupied = false;
                for (const auto& [id, op] : operations_) {
                    (void)id;
                    if ((op.state == OperationState::Placed || op.state == OperationState::Active) &&
                        closeModules(r, op.module)) { occupied = true; break; }
                }
                if (occupied) continue;
                auto targets = dockingPoints(r, input_ids.size());
                if (targets.size() != input_ids.size()) continue;
                bool valid = true;
                for (std::size_t i = 0; i < targets.size(); ++i) {
                    if (router_.blocked(targets[i])) valid = false;
                    for (std::size_t j = i + 1; j < targets.size(); ++j)
                        if (chebyshev(targets[i], targets[j]) < 2) valid = false;
                }
                if (!valid) continue;
                auto center = r.center();
                double score = std::abs(center.x - cx) + std::abs(center.y - cy);
                if (score < best_score) { best_score = score; best = r; }
            }
        }
        return best;
    }

    static std::vector<Point> dockingPoints(Rect r, std::size_t count) {
        if (count == 1) return {r.center()};
        if (count == 2) return {{r.x, r.y}, {r.x + 2, r.y + 2}};
        return {};
    }

    void placeReady() {
        for (auto& [id, op] : operations_) {
            (void)id;
            if (op.state != OperationState::Pending) continue;
            auto inputs = readyInputs(op);
            if (!inputs || inputs->empty()) continue;
            auto location = findPlacement(*inputs);
            if (!location) continue;
            op.module = *location;
            op.input_droplets = *inputs;
            auto docks = dockingPoints(*location, inputs->size());
            for (std::size_t i = 0; i < inputs->size(); ++i) router_.setTarget((*inputs)[i], docks[i]);
            op.state = OperationState::Placed;
        }
    }

    void startArrived() {
        for (auto& [id, op] : operations_) {
            (void)id;
            if (op.state != OperationState::Placed) continue;
            if (!std::all_of(op.input_droplets.begin(), op.input_droplets.end(),
                             [&](int droplet) { return router_.arrived(droplet); })) continue;
            if (!router_.moduleClearExcept(op.module, op.input_droplets)) continue;
            for (int droplet : op.input_droplets) router_.removeDroplet(droplet);
            router_.changeBlock(op.module, true);
            op.state = OperationState::Active;
            op.remaining = op.duration;
        }
    }

    void finishOperations() {
        struct BranchActivation { int source_id; int new_id; OperationType type; };
        std::vector<BranchActivation> to_activate;
        for (auto& [id, op] : operations_) {
            if (op.state != OperationState::Active || --op.remaining > 0) continue;
            router_.changeBlock(op.module, false);
            if (op.type == OperationType::Mix || op.type == OperationType::Detect) {
                op.output_droplet = router_.addDroplet(op.module.center());
            }
            op.state = OperationState::Done;
            ++completed_operations_;
            auto branch = branches_.find(id);
            if (branch != branches_.end()) {
                to_activate.push_back({id, branch->second.first, branch->second.second});
                branches_.erase(branch);
            }
        }
        for (const auto& branch : to_activate) {
            appendBlock({Operation{branch.new_id, branch.type,
                                   {Input{branch.source_id, 0}}, 2}});
        }
    }
};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void selfTest() {
    {
        Router r(12, 12);
        int d = r.addDroplet({1, 1});
        r.changeBlock({4, 0, 2, 8}, true);
        r.setTarget(d, {9, 1});
        for (int i = 0; i < 80 && !r.arrived(d); ++i) r.step();
        require(r.arrived(d), "BFS routing around blockage failed");
    }
    {
        Router r(12, 12);
        int a = r.addDroplet({1, 1});
        int b = r.addDroplet({10, 10});
        r.setTarget(a, {9, 1});
        r.setTarget(b, {2, 10});
        for (int i = 0; i < 20; ++i) {
            r.step(0.15);
            require(r.separationValid(), "router violated separation");
        }
    }
    {
        Router r(12, 12);
        int d = r.addDroplet({1, 1});
        r.setTarget(d, {9, 1});
        r.step();
        r.changeBlock({5, 0, 2, 8}, true);
        for (int i = 0; i < 80 && !r.arrived(d); ++i) r.step();
        require(r.arrived(d), "routing after blockage update failed");
    }
    std::cout << "self-test: passed\n";
}

void demo() {
    Simulator sim(30, 30);
    int a = sim.router().addDroplet({0, 0});
    int b = sim.router().addDroplet({0, 8});
    int c = sim.router().addDroplet({0, 16});
    int d = sim.router().addDroplet({0, 24});
    sim.appendBlock({Operation{1, OperationType::Mix, {{0, a}, {0, b}}, 8},
                     Operation{2, OperationType::Mix, {{0, c}, {0, d}}, 15},
                     Operation{3, OperationType::Detect, {{1, 0}}, 5},
                     Operation{4, OperationType::Detect, {{2, 0}}, 5}});
    sim.setDetectionBranch(3, 1003, OperationType::Output);
    sim.setDetectionBranch(4, 1004, OperationType::Waste);
    for (int i = 0; i < 500 && !sim.finished(); ++i) sim.tick();
    require(sim.finished(), "demo did not finish within 500 cycles");
    std::cout << "cycles=" << sim.cycles()
              << " completed_operations=" << sim.completedOperations()
              << " activated_blocks=" << sim.activatedBlocks()
              << " completed_routes=" << sim.router().completedRoutes()
              << " router_decision_ms_total=" << sim.router().decisionMilliseconds() << '\n';
}

} // namespace dmf

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--self-test") dmf::selfTest();
        else if (argc == 1 || (argc == 2 && std::string(argv[1]) == "--demo")) dmf::demo();
        else { std::cerr << "usage: am_dmf [--demo|--self-test]\n"; return 2; }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
