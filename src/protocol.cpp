#include "dmf/protocol.hpp"

#include <queue>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace dmf {

InputRef InputRef::entry(std::size_t index) {
    return {Source::BlockEntry, index, {}};
}

InputRef InputRef::output(std::string operation, std::size_t index) {
    return {Source::OperationOutput, index, std::move(operation)};
}

const BlockSpec& Protocol::block(const std::string& id) const {
    const auto found = blocks.find(id);
    if (found == blocks.end()) throw std::invalid_argument("unknown block: " + id);
    return found->second;
}

void Protocol::validate() const {
    if (blocks.find(start_block) == blocks.end()) {
        throw std::invalid_argument("start block is missing");
    }

    for (const auto& [block_id, block_spec] : blocks) {
        if (block_id.empty() || block_spec.id != block_id) {
            throw std::invalid_argument("block id does not match its map key");
        }

        std::map<std::string, const OperationSpec*> operations;
        for (const auto& operation : block_spec.operations) {
            if (operation.id.empty() || !operations.emplace(operation.id, &operation).second) {
                throw std::invalid_argument("empty or duplicate operation id in block: " + block_id);
            }
            if ((operation.type == OperationType::Dispense &&
                 (!operation.inputs.empty() || operation.output_count != 1)) ||
                (operation.type == OperationType::Split &&
                 (operation.inputs.size() != 1 || operation.output_count != 2))) {
                throw std::invalid_argument("invalid dispense or split input/output count");
            }
        }

        std::map<std::string, std::size_t> indegree;
        std::map<std::string, std::vector<std::string>> successors;
        for (const auto& [id, ignored] : operations) {
            (void)ignored;
            indegree.emplace(id, 0);
        }

        for (const auto& operation : block_spec.operations) {
            std::set<std::tuple<InputRef::Source, std::string, std::size_t>> used_inputs;
            for (const auto& input : operation.inputs) {
                if (!used_inputs.emplace(input.source, input.operation, input.index).second) {
                    throw std::invalid_argument("operation uses one input droplet twice");
                }
                if (input.source == InputRef::Source::BlockEntry) {
                    if (input.index >= block_spec.entry_count) {
                        throw std::invalid_argument("block entry index is out of range");
                    }
                    continue;
                }

                const auto producer = operations.find(input.operation);
                if (producer == operations.end() || input.index >= producer->second->output_count) {
                    throw std::invalid_argument("invalid operation output reference in block: " + block_id);
                }
                ++indegree.at(operation.id);
                successors[input.operation].push_back(operation.id);
            }
        }

        std::queue<std::string> ready;
        for (const auto& [id, count] : indegree) {
            if (count == 0) ready.push(id);
        }
        std::size_t visited = 0;
        while (!ready.empty()) {
            const std::string id = ready.front();
            ready.pop();
            ++visited;
            for (const auto& next : successors[id]) {
                if (--indegree.at(next) == 0) ready.push(next);
            }
        }
        if (visited != operations.size()) {
            throw std::invalid_argument("operation DAG contains a cycle in block: " + block_id);
        }

        std::set<std::string> decided_operations;
        for (const auto& decision : block_spec.decisions) {
            const auto operation = operations.find(decision.operation);
            if (operation == operations.end() ||
                operation->second->type != OperationType::Detect) {
                throw std::invalid_argument("decision must refer to a detect operation");
            }
            if (!decided_operations.insert(decision.operation).second) {
                throw std::invalid_argument("detect operation has duplicate decisions");
            }
            if (decision.outcomes.empty()) {
                throw std::invalid_argument("decision has no outcomes");
            }
            for (const auto& [outcome, branch] : decision.outcomes) {
                if (outcome.empty()) throw std::invalid_argument("empty decision outcome");
                const auto target = blocks.find(branch.target_block);
                if (target == blocks.end()) {
                    throw std::invalid_argument("decision target is missing");
                }
                if (branch.entry_output_indices.size() != target->second.entry_count) {
                    throw std::invalid_argument("decision does not supply target block entries");
                }
                const std::set<std::size_t> unique_outputs(
                    branch.entry_output_indices.begin(), branch.entry_output_indices.end());
                if (unique_outputs.size() != branch.entry_output_indices.size()) {
                    throw std::invalid_argument("one droplet cannot fill two block entries");
                }
                for (const auto index : branch.entry_output_indices) {
                    if (index >= operation->second->output_count) {
                        throw std::invalid_argument("decision output index is out of range");
                    }
                }
            }
        }
    }
}

}  // namespace dmf
