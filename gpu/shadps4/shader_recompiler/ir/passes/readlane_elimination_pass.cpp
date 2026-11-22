// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <bit>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/program.h"

// resogun: SGPR spills. The GCN compiler parks an SGPR in one lane of a VGPR (v_writelane_b32)
// and reloads it later (v_readlane_b32 with a constant lane). Per the GCN3 ISA both ignore EXEC:
// v_writelane writes only lane S1[5:0] of the destination VGPR and every other lane keeps its
// value; v_readlane returns lane S1[5:0] of the source VGPR as a scalar. A Vulkan subgroup may be
// 32 wide and the lane may be inactive, so a reload cannot simply be a broadcast from lane N. This
// pass rewrites each reload with a constant lane into the scalar value parked there, following
// the VGPR's SSA history:
//  - a v_writelane to the same lane gives the written value;
//  - a v_writelane to another constant lane is looked through;
//  - a phi becomes a new phi of the lane's value on each incoming edge (one per phi and lane);
//  - an immediate, an undefined value or a lane read result is the same in every lane;
//  - anything else (the VGPR carries some other vector value on that path, or a v_writelane whose
//    lane is not a constant) keeps a real lane read of that value, placed where the value is
//    live: before the original read, or at the end of the phi's incoming block.
// Every v_writelane left afterwards is lowered to its ISA meaning on the invocation's own copy of
// the VGPR: lane_id == lane ? written : old.

namespace Shader::Optimization {
namespace {

constexpr u32 LaneMask = 63; // the lane select is bits 5:0

struct Site {
    IR::Block* block;
    IR::Block::iterator pos;
};

class LaneValues {
public:
    // The value lane `lane` of `value` holds at `site`.
    IR::Value Resolve(const IR::Value& value, u32 lane, Site site) {
        if (value.IsImmediate()) {
            return AsU32(value);
        }
        IR::Inst* inst = value.Inst();
        while (inst->GetOpcode() == IR::Opcode::WriteLane) {
            const IR::Value target = inst->Arg(2);
            if (!target.IsImmediate()) {
                return LaneRead(IR::Value{inst}, lane, site);
            }
            if ((target.U32() & LaneMask) == lane) {
                return inst->Arg(1);
            }
            const IR::Value older = inst->Arg(0);
            if (older.IsImmediate()) {
                return AsU32(older);
            }
            inst = older.Inst();
        }
        switch (inst->GetOpcode()) {
        case IR::Opcode::Phi:
            return LanePhi(inst, lane);
        case IR::Opcode::UndefU32:
        case IR::Opcode::ReadLane:
        case IR::Opcode::ReadFirstLane:
            return IR::Value{inst};
        default:
            return LaneRead(IR::Value{inst}, lane, site);
        }
    }

    // True when every value `value` can come from is 32 bits wide (what Resolve handles).
    static bool Resolvable(const IR::Value& value) {
        std::vector<IR::Value> stack{value};
        std::unordered_set<IR::Inst*> seen;
        while (!stack.empty()) {
            const IR::Value v = stack.back();
            stack.pop_back();
            if (!v.IsImmediate()) {
                IR::Inst* const inst = v.Inst();
                if (!seen.insert(inst).second) {
                    continue;
                }
                if (inst->GetOpcode() == IR::Opcode::WriteLane) {
                    stack.push_back(inst->Arg(0));
                    continue;
                }
                if (inst->GetOpcode() == IR::Opcode::Phi) {
                    for (size_t i = 0; i < inst->NumArgs(); ++i) {
                        stack.push_back(inst->Arg(i));
                    }
                    continue;
                }
            }
            if (v.Type() != IR::Type::U32 && v.Type() != IR::Type::F32) {
                return false;
            }
        }
        return true;
    }

    std::vector<IR::Inst*> created_phis;

private:
    static IR::Value AsU32(const IR::Value& imm) {
        return imm.Type() == IR::Type::F32 ? IR::Value{std::bit_cast<u32>(imm.F32())} : imm;
    }

    static IR::Value LaneRead(const IR::Value& value, u32 lane, Site site) {
        IR::IREmitter ir{*site.block, site.pos};
        const IR::U32 bits = value.Type() == IR::Type::F32
                                 ? ir.BitCast<IR::U32, IR::F32>(IR::F32{value})
                                 : IR::U32{value};
        return ir.ReadLane(bits, ir.Imm32(lane));
    }

    IR::Value LanePhi(IR::Inst* phi, u32 lane) {
        const auto key = std::make_pair(phi, lane);
        if (const auto it = lane_phis.find(key); it != lane_phis.end()) {
            return IR::Value{it->second};
        }
        IR::Block* const block = phi->GetParent();
        IR::Inst* const lane_phi = &*block->PrependNewInst(
            IR::Block::InstructionList::s_iterator_to(*phi), IR::Opcode::Phi);
        lane_phi->SetFlags(IR::Type::U32);
        lane_phis.emplace(key, lane_phi);
        created_phis.push_back(lane_phi);
        // Resolve every edge before adding operands: an edge may come back to this phi.
        std::vector<std::pair<IR::Block*, IR::Value>> edges;
        for (size_t i = 0; i < phi->NumArgs(); ++i) {
            IR::Block* const from = phi->PhiBlock(i);
            edges.emplace_back(from, Resolve(phi->Arg(i), lane, Site{from, from->end()}));
        }
        for (const auto& [from, value] : edges) {
            lane_phi->AddPhiOperand(from, value);
        }
        return IR::Value{lane_phi};
    }

    struct KeyHash {
        size_t operator()(const std::pair<IR::Inst*, u32>& k) const {
            return std::hash<IR::Inst*>{}(k.first) ^ (size_t{k.second} << 1);
        }
    };
    std::unordered_map<std::pair<IR::Inst*, u32>, IR::Inst*, KeyHash> lane_phis;
};

// Lane phis that merge one value (besides themselves) become that value.
void FoldLanePhis(std::vector<IR::Inst*>& phis) {
    for (bool changed = true; changed;) {
        changed = false;
        for (IR::Inst*& phi : phis) {
            if (phi == nullptr) {
                continue;
            }
            IR::Value only;
            bool merges = false;
            for (size_t i = 0; i < phi->NumArgs() && !merges; ++i) {
                const IR::Value arg = phi->Arg(i);
                if (arg == IR::Value{phi} || arg == only) {
                    continue;
                }
                merges = !only.IsEmpty();
                only = arg;
            }
            if (merges || only.IsEmpty()) {
                continue;
            }
            IR::Block* const block = phi->GetParent();
            phi->ReplaceUsesWithAndRemove(only);
            block->Instructions().erase(IR::Block::InstructionList::s_iterator_to(*phi));
            phi = nullptr;
            changed = true;
        }
    }
}

void LowerWriteLanes(IR::Program& program) {
    for (IR::Block* const block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() != IR::Opcode::WriteLane) {
                continue;
            }
            IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
            const IR::Value target = inst.Arg(2);
            const IR::U32 lane =
                target.IsImmediate()
                    ? ir.Imm32(target.U32() & LaneMask)
                    : IR::U32{ir.BitwiseAnd(IR::U32{target}, ir.Imm32(LaneMask))};
            const IR::U1 mine = ir.IEqual(ir.LaneId(), lane);
            const IR::U32 merged{ir.Select(mine, IR::U32{inst.Arg(1)}, IR::U32{inst.Arg(0)})};
            inst.ReplaceUsesWithAndRemove(merged);
        }
    }
}

} // namespace

void ReadLaneEliminationPass(IR::Program& program) {
    std::vector<IR::Inst*> reads;
    for (IR::Block* const block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() == IR::Opcode::ReadLane && inst.Arg(1).IsImmediate() &&
                !inst.Arg(0).IsImmediate()) {
                reads.push_back(&inst);
            }
        }
    }
    LaneValues lanes;
    for (IR::Inst* const read : reads) {
        const IR::Value source = read->Arg(0);
        const IR::Opcode producer = source.Inst()->GetOpcode();
        if (producer != IR::Opcode::WriteLane && producer != IR::Opcode::Phi) {
            continue; // a plain vector value: the read stays a lane read
        }
        if (!LaneValues::Resolvable(source)) {
            continue;
        }
        const u32 lane = read->Arg(1).U32() & LaneMask;
        const Site site{read->GetParent(), IR::Block::InstructionList::s_iterator_to(*read)};
        read->ReplaceUsesWith(lanes.Resolve(source, lane, site));
    }
    FoldLanePhis(lanes.created_phis);
    LowerWriteLanes(program);
}

} // namespace Shader::Optimization
