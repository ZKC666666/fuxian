#include "custom_route_case.hpp"

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>

namespace dmf::demo {

namespace {

void checkEnd(std::istringstream& row, int line_number) {
    std::string extra;
    if (row >> extra) {
        throw std::invalid_argument("line " + std::to_string(line_number) +
                                    ": unexpected extra value");
    }
}

[[noreturn]] void badLine(int line_number, const std::string& reason) {
    throw std::invalid_argument("line " + std::to_string(line_number) + ": " + reason);
}

}  // namespace

RouteCase parseRouteCase(std::istream& input) {
    RouteCase result;
    bool has_chip = false;
    bool has_separation = false;
    bool has_max_steps = false;
    bool has_trace = false;
    std::string line;
    int line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        if (line_number == 1 && line.compare(0, 3, "\xEF\xBB\xBF") == 0) {
            line.erase(0, 3);  // Accept a UTF-8 BOM from text editors.
        }
        const auto comment = line.find('#');
        if (comment != std::string::npos) line.erase(comment);
        std::istringstream row(line);
        std::string key;
        if (!(row >> key)) continue;

        if (key == "chip") {
            if (has_chip || !(row >> result.width >> result.height)) {
                badLine(line_number, "expected one chip width and height");
            }
            has_chip = true;
        } else if (key == "separation") {
            if (has_separation || !(row >> result.separation)) {
                badLine(line_number, "expected one separation value");
            }
            has_separation = true;
        } else if (key == "max_steps") {
            if (has_max_steps || !(row >> result.max_steps)) {
                badLine(line_number, "expected one max_steps value");
            }
            has_max_steps = true;
        } else if (key == "trace") {
            int enabled = -1;
            if (has_trace || !(row >> enabled) || (enabled != 0 && enabled != 1)) {
                badLine(line_number, "trace must be 0 or 1");
            }
            result.trace = enabled == 1;
            has_trace = true;
        } else if (key == "droplet") {
            DropletId droplet = 0;
            Point start{0, 0};
            Point target{0, 0};
            if (!(row >> droplet >> start.x >> start.y >> target.x >> target.y) ||
                droplet <= 0 || !result.positions.emplace(droplet, start).second) {
                badLine(line_number, "expected a positive unique droplet ID and four coordinates");
            }
            result.tasks.push_back({droplet, droplet, target, 0});
        } else {
            badLine(line_number, "unknown setting: " + key);
        }
        checkEnd(row, line_number);
    }
    if (!input.eof() && input.fail()) {
        throw std::runtime_error("could not read routing case");
    }
    if (!has_chip || result.width <= 0 || result.height <= 0 ||
        static_cast<long long>(result.width) * result.height > 90000) {
        throw std::invalid_argument("chip must have 1 to 90000 electrodes");
    }
    if (result.separation < 1 || result.max_steps < 1 || result.max_steps > 100000) {
        throw std::invalid_argument("separation or max_steps is outside the supported range");
    }
    if (result.tasks.empty() || result.tasks.size() > 400) {
        throw std::invalid_argument("case must contain 1 to 400 droplets");
    }
    for (const auto& task : result.tasks) {
        const auto start = result.positions.at(task.droplet);
        const auto in_bounds = [&](Point point) {
            return point.x >= 0 && point.x < result.width &&
                   point.y >= 0 && point.y < result.height;
        };
        if (!in_bounds(start) || !in_bounds(task.target)) {
            throw std::invalid_argument("droplet " + std::to_string(task.droplet) +
                                        " has a position outside the chip");
        }
    }
    for (auto first = result.positions.begin(); first != result.positions.end(); ++first) {
        for (auto second = std::next(first); second != result.positions.end(); ++second) {
            const int dx = std::abs(first->second.x - second->second.x);
            const int dy = std::abs(first->second.y - second->second.y);
            if (std::max(dx, dy) < result.separation) {
                throw std::invalid_argument(
                    "starting droplets " + std::to_string(first->first) + " and " +
                    std::to_string(second->first) + " violate separation");
            }
        }
    }
    return result;
}

}  // namespace dmf::demo
