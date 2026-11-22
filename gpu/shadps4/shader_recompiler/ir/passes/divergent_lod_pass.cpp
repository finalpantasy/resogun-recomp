// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/program.h"

// resogun: implicit-LOD samples in divergent control flow. Vulkan leaves derivatives undefined
// where the invocations of a quad do not all run (GCN reads whatever the inactive lanes hold), so
// the LOD of an implicit sample there is garbage; in Everybody's Golf that drew horizontal lines in
// the shadows. This pass finds those samples and marks them (TextureInstInfo::divergent_lod); the
// SPIR-V backend then takes level 0 explicitly when the bound view has a single level, the case
// where the LOD only chooses between the min and mag filters.
//
// Divergence is a forward taint over SSA values on the structured program: a value is varying
// when it comes from a per-invocation source (attributes, lane id, undefined values, shared memory,
// atomics) or from a varying operand, or when it is a phi where divergent control flow merges.
// A conditional with a varying condition, a loop with a varying exit, and everything after a
// return taken in divergent control flow, run divergently.

namespace Shader::Optimization {
namespace {

using Type = IR::AbstractSyntaxNode::Type;

bool IsVaryingSource(const IR::Inst& inst) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::GetAttribute:
    case IR::Opcode::GetAttributeU1:
    case IR::Opcode::GetAttributeU32:
    case IR::Opcode::GetPatch:
    case IR::Opcode::GetTessGenericAttribute:
    case IR::Opcode::GetVectorRegister:
    case IR::Opcode::GetVectorRegisterIndexed:
    case IR::Opcode::ReadVgprArray:
    case IR::Opcode::LaneId:
    case IR::Opcode::UndefU1:
    case IR::Opcode::UndefU8:
    case IR::Opcode::UndefU16:
    case IR::Opcode::UndefU32:
    case IR::Opcode::UndefU64:
    case IR::Opcode::UndefF32:
    case IR::Opcode::LoadSharedU16:
    case IR::Opcode::LoadSharedU32:
    case IR::Opcode::LoadSharedU64:
        return true;
    default:
        return inst.MayHaveSideEffects() && inst.HasUses(); // atomics return per-invocation data
    }
}

// Subgroup operations whose result is the same in every invocation.
bool IsUniformResult(IR::Opcode op) {
    switch (op) {
    case IR::Opcode::ReadFirstLane:
    case IR::Opcode::ReadLane:
    case IR::Opcode::Ballot:
    case IR::Opcode::BallotFindLsb:
    case IR::Opcode::GroupAny:
        return true;
    default:
        return false;
    }
}

class Divergence {
public:
    explicit Divergence(IR::Program& program_) : program{program_} {
        for (IR::Block* const block : program.blocks) {
            for (IR::Inst& inst : block->Instructions()) {
                if (IsVaryingSource(inst)) {
                    varying.insert(&inst);
                }
            }
        }
        // Values feed blocks and blocks feed values (phis at divergent merges): iterate.
        do {
            Propagate();
        } while (MarkDivergentBlocks());
    }

    bool InDivergentFlow(const IR::Block* block) const {
        return divergent_blocks.contains(block);
    }

private:
    bool IsVarying(const IR::Value& value) const {
        return !value.IsImmediate() && varying.contains(value.Inst());
    }

    void Propagate() {
        for (bool changed = true; changed;) {
            changed = false;
            for (IR::Block* const block : program.blocks) {
                const bool divergent_merge = merge_blocks.contains(block);
                for (IR::Inst& inst : block->Instructions()) {
                    if (varying.contains(&inst) || IsUniformResult(inst.GetOpcode())) {
                        continue;
                    }
                    bool becomes = inst.GetOpcode() == IR::Opcode::Phi && divergent_merge;
                    for (size_t i = 0; i < inst.NumArgs() && !becomes; ++i) {
                        becomes = IsVarying(inst.Arg(i));
                    }
                    if (becomes) {
                        varying.insert(&inst);
                        changed = true;
                    }
                }
            }
        }
    }

    // Walks the syntax list; returns true when it found new divergent merge blocks.
    bool MarkDivergentBlocks() {
        const auto& list = program.syntax_list;
        // Loops (by merge block) whose exit, a break or the repeat condition, is varying, and
        // their headers, where the values carried around the loop meet.
        std::unordered_set<const IR::Block*> divergent_loops;
        std::unordered_map<const IR::Block*, IR::Block*> headers;
        for (const IR::AbstractSyntaxNode& node : list) {
            if (node.type == Type::Break && IsVarying(node.data.break_node.cond)) {
                divergent_loops.insert(node.data.break_node.merge);
            } else if (node.type == Type::Repeat) {
                headers.emplace(node.data.repeat.merge, node.data.repeat.loop_header);
                if (IsVarying(node.data.repeat.cond)) {
                    divergent_loops.insert(node.data.repeat.merge);
                }
            }
        }
        const size_t merges_before = merge_blocks.size();
        divergent_blocks.clear();
        std::vector<bool> scopes; // per open if/loop: divergent
        u32 depth = 0;
        bool after_divergent_return = false;
        const auto open = [&](bool divergent) {
            scopes.push_back(divergent);
            depth += divergent;
        };
        const auto close = [&] {
            depth -= scopes.back();
            scopes.pop_back();
        };
        for (const IR::AbstractSyntaxNode& node : list) {
            switch (node.type) {
            case Type::If: {
                const bool divergent = IsVarying(node.data.if_node.cond);
                if (divergent) {
                    merge_blocks.insert(node.data.if_node.merge);
                }
                open(divergent);
                break;
            }
            case Type::Loop: {
                const bool divergent = divergent_loops.contains(node.data.loop.merge);
                if (divergent) {
                    if (const auto it = headers.find(node.data.loop.merge); it != headers.end()) {
                        merge_blocks.insert(it->second);
                    }
                    merge_blocks.insert(node.data.loop.body);
                    merge_blocks.insert(node.data.loop.merge);
                }
                open(divergent);
                break;
            }
            case Type::EndIf:
            case Type::Repeat:
                if (!scopes.empty()) {
                    close();
                }
                break;
            case Type::Return:
            case Type::Unreachable:
                after_divergent_return |= depth > 0;
                break;
            case Type::Block:
                if (depth > 0 || after_divergent_return) {
                    divergent_blocks.insert(node.data.block);
                }
                break;
            default:
                break;
            }
        }
        return merge_blocks.size() != merges_before;
    }

    IR::Program& program;
    std::unordered_set<const IR::Inst*> varying;
    std::unordered_set<const IR::Block*> merge_blocks;
    std::unordered_set<const IR::Block*> divergent_blocks;
};

} // namespace

void DivergentLodPass(IR::Program& program) {
    if (program.info.sw_stage != SwStage::Fragment) {
        return;
    }
    const Divergence divergence{program};
    for (IR::Block* const block : program.blocks) {
        if (!divergence.InDivergentFlow(block)) {
            continue;
        }
        for (IR::Inst& inst : block->Instructions()) {
            const IR::Opcode op = inst.GetOpcode();
            if (op != IR::Opcode::ImageSampleImplicitLod &&
                op != IR::Opcode::ImageSampleDrefImplicitLod) {
                continue;
            }
            auto flags = inst.Flags<IR::TextureInstInfo>();
            flags.divergent_lod.Assign(1);
            inst.SetFlags(flags);
        }
    }
}

} // namespace Shader::Optimization
