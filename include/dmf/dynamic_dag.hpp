#pragma once

#include "dmf/protocol.hpp"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace dmf {

using DropletId = int;
using BlockInstanceId = int;
using OperationInstanceId = int;

enum class OperationState { Waiting, Running, Completed };

struct OperationInstance {
    OperationInstanceId id;
    BlockInstanceId block_instance;
    std::string spec_id;
    OperationState state = OperationState::Waiting;
    std::vector<DropletId> outputs;
};

// The runtime graph contains only blocks activated by feedback. A repeated
// activation creates new operation instances, even for the same CFG block.
class DynamicDag {
public:
    explicit DynamicDag(Protocol protocol);

    BlockInstanceId activateStart(std::vector<DropletId> entry_droplets);
    std::vector<OperationInstanceId> readyOperations() const;
    std::vector<DropletId> startOperation(OperationInstanceId id);
    void finishOperation(OperationInstanceId id, std::vector<DropletId> outputs,
                         std::optional<std::string> outcome = std::nullopt);
    std::vector<BlockInstanceId> advanceCycle();

    int cycle() const { return cycle_; }
    std::size_t blockCount() const { return blocks_.size(); }
    std::size_t operationCount() const { return operations_.size(); }
    bool allDone() const;
    OperationInstanceId operationFor(BlockInstanceId block, const std::string& local_id) const;
    const OperationInstance& operation(OperationInstanceId id) const;
    const std::string& blockName(BlockInstanceId id) const;

private:
    struct BlockInstance {
        BlockInstanceId id;
        std::string spec_id;
        std::vector<DropletId> entries;
        std::map<std::string, OperationInstanceId> operations;
    };

    struct PendingBranch {
        std::string target_block;
        std::vector<DropletId> entries;
    };

    Protocol protocol_;
    int cycle_ = 0;
    int next_block_id_ = 1;
    int next_operation_id_ = 1;
    std::map<BlockInstanceId, BlockInstance> blocks_;
    std::map<OperationInstanceId, OperationInstance> operations_;
    std::set<DropletId> known_droplets_;
    std::set<DropletId> available_droplets_;
    std::vector<DropletId> pending_outputs_;
    std::vector<PendingBranch> pending_branches_;

    BlockInstanceId addBlock(const std::string& spec_id, std::vector<DropletId> entries);
    const OperationSpec& specFor(const OperationInstance& instance) const;
    std::optional<std::vector<DropletId>> resolvedInputs(const OperationInstance& instance) const;
};

}  // namespace dmf
