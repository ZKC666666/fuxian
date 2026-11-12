#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace dmf {

enum class OperationType { Mix, Detect, Output, Waste, Dispense, Split };

// A block entry is supplied when the block is activated. An operation output
// refers to a droplet produced by another operation in the same block.
struct InputRef {
    enum class Source { BlockEntry, OperationOutput };

    Source source;
    std::size_t index;
    std::string operation;

    static InputRef entry(std::size_t index);
    static InputRef output(std::string operation, std::size_t index = 0);
};

struct OperationSpec {
    std::string id;
    OperationType type;
    std::vector<InputRef> inputs;
    std::size_t output_count;
};

struct BranchSpec {
    std::string target_block;
    std::vector<std::size_t> entry_output_indices;
};

struct DecisionSpec {
    std::string operation;
    std::map<std::string, BranchSpec> outcomes;
};

struct BlockSpec {
    std::string id;
    std::size_t entry_count;
    std::vector<OperationSpec> operations;
    std::vector<DecisionSpec> decisions;
};

// CFG: possible block transitions. Each block contains its own fixed DAG.
struct Protocol {
    std::string start_block;
    std::map<std::string, BlockSpec> blocks;

    const BlockSpec& block(const std::string& id) const;
    void validate() const;
};

}  // namespace dmf
