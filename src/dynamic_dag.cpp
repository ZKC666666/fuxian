#include "dmf/dynamic_dag.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace dmf {

DynamicDag::DynamicDag(Protocol protocol) : protocol_(std::move(protocol)) {
    protocol_.validate();
}

BlockInstanceId DynamicDag::activateStart(std::vector<DropletId> entry_droplets) {
    if (!blocks_.empty()) throw std::logic_error("start block was already activated");
    if (entry_droplets.size() != protocol_.block(protocol_.start_block).entry_count) {
        throw std::invalid_argument("wrong number of start droplets");
    }
    std::set<DropletId> unique(entry_droplets.begin(), entry_droplets.end());
    if (unique.size() != entry_droplets.size() ||
        std::any_of(entry_droplets.begin(), entry_droplets.end(), [](DropletId id) { return id <= 0; })) {
        throw std::invalid_argument("start droplet ids must be positive and distinct");
    }
    known_droplets_.insert(unique.begin(), unique.end());
    available_droplets_.insert(unique.begin(), unique.end());
    return addBlock(protocol_.start_block, std::move(entry_droplets));
}

BlockInstanceId DynamicDag::addBlock(const std::string& spec_id,
                                     std::vector<DropletId> entries) {
    const auto& spec = protocol_.block(spec_id);
    if (entries.size() != spec.entry_count) throw std::invalid_argument("wrong block entry count");
    const BlockInstanceId block_id = next_block_id_++;
    BlockInstance instance{block_id, spec_id, std::move(entries), {}};
    for (const auto& operation : spec.operations) {
        const OperationInstanceId id = next_operation_id_++;
        instance.operations.emplace(operation.id, id);
        operations_.emplace(id, OperationInstance{id, block_id, operation.id});
    }
    blocks_.emplace(block_id, std::move(instance));
    return block_id;
}

const OperationSpec& DynamicDag::specFor(const OperationInstance& instance) const {
    const auto& block = protocol_.block(blocks_.at(instance.block_instance).spec_id);
    const auto found = std::find_if(block.operations.begin(), block.operations.end(),
                                    [&](const OperationSpec& spec) { return spec.id == instance.spec_id; });
    return *found;  // Protocol validation guarantees this reference exists.
}

std::optional<std::vector<DropletId>> DynamicDag::resolvedInputs(
    const OperationInstance& instance) const {
    if (instance.state != OperationState::Waiting) return std::nullopt;
    const auto& block = blocks_.at(instance.block_instance);
    std::vector<DropletId> inputs;
    for (const auto& input : specFor(instance).inputs) {
        DropletId id = 0;
        if (input.source == InputRef::Source::BlockEntry) {
            id = block.entries.at(input.index);
        } else {
            const auto& producer = operations_.at(block.operations.at(input.operation));
            if (producer.state != OperationState::Completed) return std::nullopt;
            id = producer.outputs.at(input.index);
        }
        if (available_droplets_.count(id) == 0) return std::nullopt;
        inputs.push_back(id);
    }
    const std::set<DropletId> unique(inputs.begin(), inputs.end());
    if (unique.size() != inputs.size()) return std::nullopt;
    return inputs;
}

std::vector<OperationInstanceId> DynamicDag::readyOperations() const {
    std::vector<OperationInstanceId> ready;
    for (const auto& [id, instance] : operations_) {
        if (resolvedInputs(instance)) ready.push_back(id);
    }
    return ready;
}

std::optional<std::vector<DropletId>> DynamicDag::inputsIfReady(
    OperationInstanceId id) const {
    return resolvedInputs(operations_.at(id));
}

std::vector<DropletId> DynamicDag::reserveOperation(OperationInstanceId id) {
    auto& instance = operations_.at(id);
    auto inputs = resolvedInputs(instance);
    if (!inputs) throw std::logic_error("operation is not ready");
    for (const auto droplet : *inputs) available_droplets_.erase(droplet);
    instance.inputs = *inputs;
    instance.state = OperationState::Placed;
    return *inputs;
}

std::vector<DropletId> DynamicDag::startOperation(OperationInstanceId id) {
    auto& instance = operations_.at(id);
    if (instance.state != OperationState::Placed) {
        throw std::logic_error("operation has not been placed");
    }
    instance.state = OperationState::Running;
    return instance.inputs;
}

void DynamicDag::finishOperation(OperationInstanceId id, std::vector<DropletId> outputs,
                                 std::optional<std::string> outcome) {
    auto& instance = operations_.at(id);
    if (instance.state != OperationState::Running) {
        throw std::logic_error("operation is not running");
    }
    const auto& spec = specFor(instance);
    if (outputs.size() != spec.output_count) {
        throw std::invalid_argument("wrong number of operation outputs");
    }
    const std::set<DropletId> unique(outputs.begin(), outputs.end());
    if (unique.size() != outputs.size()) {
        throw std::invalid_argument("duplicate operation output ids");
    }
    for (const auto droplet : outputs) {
        if (droplet <= 0 || known_droplets_.count(droplet) != 0) {
            throw std::invalid_argument("operation output id is not new");
        }
    }

    const auto& block_spec = protocol_.block(blocks_.at(instance.block_instance).spec_id);
    const bool is_decision = block_spec.decision && block_spec.decision->operation == spec.id;
    if (is_decision != outcome.has_value()) {
        throw std::invalid_argument("decision outcome is missing or unexpected");
    }
    std::optional<PendingBranch> branch_to_add;
    if (is_decision) {
        const auto selected = block_spec.decision->outcomes.find(*outcome);
        if (selected == block_spec.decision->outcomes.end()) {
            throw std::invalid_argument("unknown decision outcome");
        }
        PendingBranch branch{selected->second.target_block, {}};
        for (const auto index : selected->second.entry_output_indices) {
            branch.entries.push_back(outputs.at(index));
        }
        branch_to_add = std::move(branch);
    }

    instance.outputs = std::move(outputs);
    instance.state = OperationState::Completed;
    for (const auto droplet : instance.outputs) {
        known_droplets_.insert(droplet);
        pending_outputs_.push_back(droplet);
    }
    if (branch_to_add) pending_branches_.push_back(std::move(*branch_to_add));
}

std::vector<BlockInstanceId> DynamicDag::advanceCycle() {
    ++cycle_;
    available_droplets_.insert(pending_outputs_.begin(), pending_outputs_.end());
    pending_outputs_.clear();
    auto branches = std::move(pending_branches_);
    pending_branches_.clear();
    std::vector<BlockInstanceId> activated;
    for (auto& branch : branches) {
        activated.push_back(addBlock(branch.target_block, std::move(branch.entries)));
    }
    return activated;
}

bool DynamicDag::allDone() const {
    if (blocks_.empty() || !pending_branches_.empty()) return false;
    return std::all_of(operations_.begin(), operations_.end(), [](const auto& entry) {
        return entry.second.state == OperationState::Completed;
    });
}

OperationInstanceId DynamicDag::operationFor(BlockInstanceId block,
                                              const std::string& local_id) const {
    return blocks_.at(block).operations.at(local_id);
}

const OperationInstance& DynamicDag::operation(OperationInstanceId id) const {
    return operations_.at(id);
}

OperationType DynamicDag::operationType(OperationInstanceId id) const {
    return specFor(operations_.at(id)).type;
}

const std::string& DynamicDag::blockName(BlockInstanceId id) const {
    return blocks_.at(id).spec_id;
}

}  // namespace dmf
