// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>

#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/program.h"

namespace Shader::Optimization {

// resogun: v_movrels/v_movreld address a VGPR relative to a base register with M0. Lowered as a
// compare and select over every register above the base, one program grew to 150,000 selects and
// NVIDIA took up to 20 s to compile its pipelines, stalling course loads. Here the registers from
// the lowest relative base up live in a private array; a relative access is one dynamic load or
// store, and plain accesses in that range use constant indices. The array has one spare slot at
// the end that takes out-of-range relative writes; out-of-range relative reads return V0, as the
// select chain did.
//
// Programs whose chains stay small keep them: private arrays usually live in local memory on the
// GPU, while short chains stay in registers. Only programs past VgprArraySelectBudget selects use
// the array. RESOGUN_VGPR_ARRAY=0 keeps the chains everywhere and =all uses the array everywhere;
// each mode has its own shader cache version.
constexpr u32 VgprArraySelectBudget = 4096;

u32 VgprArrayMode() {
    static const u32 mode = [] {
        const char* const env = std::getenv("RESOGUN_VGPR_ARRAY");
        if (env && env[0] == '0') {
            return 1u;
        }
        if (env && env[0] == 'a') {
            return 2u;
        }
        return 0u;
    }();
    return mode;
}

// The translator's original lowering: one compare and select per register above the base.
static void LowerToSelectChain(IR::Block& block, IR::Inst& inst, u32 num_allocated_vgprs) {
    IR::IREmitter ir{block, IR::Block::InstructionList::s_iterator_to(inst)};
    const u32 reg = static_cast<u32>(inst.Arg(0).VectorReg());
    const IR::U32 index{inst.Arg(1)};
    if (inst.GetOpcode() == IR::Opcode::GetVectorRegisterIndexed) {
        IR::U32 value = ir.GetVectorReg<IR::U32>(IR::VectorReg::V0);
        for (u32 i = reg; i < num_allocated_vgprs; i++) {
            const IR::U1 cond = ir.IEqual(index, ir.Imm32(i - reg));
            value = IR::U32{
                ir.Select(cond, ir.GetVectorReg<IR::U32>(IR::VectorReg::V0 + i), value)};
        }
        inst.ReplaceUsesWithAndRemove(value);
        return;
    }
    const IR::U32 value{inst.Arg(2)};
    for (u32 i = reg; i < num_allocated_vgprs; i++) {
        const IR::U1 cond = ir.IEqual(index, ir.Imm32(i - reg));
        ir.SetVectorReg(IR::VectorReg::V0 + i,
                        IR::U32{ir.Select(cond, value,
                                          ir.GetVectorReg<IR::U32>(IR::VectorReg::V0 + i))});
    }
    inst.Invalidate();
}

void VgprArrayPass(IR::Program& program, u32 num_allocated_vgprs) {
    u32 base = std::numeric_limits<u32>::max();
    u64 selects = 0;
    // Registers that hold SGPR spills (v_writelane/v_readlane) or feed v_readfirstlane stay out of
    // the array: ReadLaneEliminationPass and sharp tracking follow them through SSA. The compiler
    // never indexes into its spill registers, so their array slots are simply unused.
    std::array<bool, IR::NumVectorRegs> lane_regs{};
    const auto mark_lane_source = [&](const IR::Value& value) {
        const IR::Inst* const source = value.TryInst();
        if (source && source->GetOpcode() == IR::Opcode::GetVectorRegister) {
            lane_regs[static_cast<u32>(source->Arg(0).VectorReg())] = true;
        }
    };
    for (IR::Block* const block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            switch (inst.GetOpcode()) {
            case IR::Opcode::GetVectorRegisterIndexed:
            case IR::Opcode::SetVectorRegisterIndexed: {
                const u32 reg = static_cast<u32>(inst.Arg(0).VectorReg());
                base = std::min(base, reg);
                selects += reg < num_allocated_vgprs ? num_allocated_vgprs - reg : 0;
                break;
            }
            case IR::Opcode::ReadLane:
            case IR::Opcode::WriteLane:
            case IR::Opcode::ReadFirstLane:
                mark_lane_source(inst.Arg(0));
                break;
            case IR::Opcode::SetVectorRegister: {
                const IR::Inst* const value = inst.Arg(1).TryInst();
                if (value && value->GetOpcode() == IR::Opcode::WriteLane) {
                    lane_regs[static_cast<u32>(inst.Arg(0).VectorReg())] = true;
                }
                break;
            }
            default:
                break;
            }
        }
    }
    if (base == std::numeric_limits<u32>::max()) {
        return;
    }
    const u32 mode = VgprArrayMode();
    if (mode == 1 || (mode == 0 && selects <= VgprArraySelectBudget)) {
        for (IR::Block* const block : program.blocks) {
            for (IR::Inst& inst : block->Instructions()) {
                const IR::Opcode op = inst.GetOpcode();
                if (op == IR::Opcode::GetVectorRegisterIndexed ||
                    op == IR::Opcode::SetVectorRegisterIndexed) {
                    LowerToSelectChain(*block, inst, num_allocated_vgprs);
                }
            }
        }
        return;
    }
    const u32 length = num_allocated_vgprs - base;
    program.info.vgpr_array_size = length + 1;

    const auto in_array = [&](IR::VectorReg reg) {
        const u32 index = static_cast<u32>(reg);
        return index >= base && index < num_allocated_vgprs && !lane_regs[index];
    };
    for (IR::Block* const block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            switch (inst.GetOpcode()) {
            case IR::Opcode::GetVectorRegister: {
                const IR::VectorReg reg = inst.Arg(0).VectorReg();
                if (in_array(reg)) {
                    inst.ReplaceOpcode(IR::Opcode::ReadVgprArray);
                    inst.SetArg(0, IR::Value{static_cast<u32>(reg) - base});
                }
                break;
            }
            case IR::Opcode::SetVectorRegister: {
                const IR::VectorReg reg = inst.Arg(0).VectorReg();
                if (in_array(reg)) {
                    inst.ReplaceOpcode(IR::Opcode::WriteVgprArray);
                    inst.SetArg(0, IR::Value{static_cast<u32>(reg) - base});
                }
                break;
            }
            case IR::Opcode::GetVectorRegisterIndexed: {
                IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
                const u32 reg = static_cast<u32>(inst.Arg(0).VectorReg());
                const IR::U32 index{inst.Arg(1)};
                const IR::U1 in_range{ir.ILessThan(index, ir.Imm32(num_allocated_vgprs - reg), false)};
                const IR::U32 slot{ir.IAdd(index, ir.Imm32(reg - base))};
                const IR::U32 value{ir.ReadVgprArray(IR::U32{ir.Select(in_range, slot, ir.Imm32(0u))})};
                const IR::U32 v0{in_array(IR::VectorReg::V0) ? ir.ReadVgprArray(ir.Imm32(0u))
                                           : ir.GetVectorReg<IR::U32>(IR::VectorReg::V0)};
                inst.ReplaceUsesWithAndRemove(ir.Select(in_range, value, v0));
                break;
            }
            case IR::Opcode::SetVectorRegisterIndexed: {
                IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
                const u32 reg = static_cast<u32>(inst.Arg(0).VectorReg());
                const IR::U32 index{inst.Arg(1)};
                const IR::U32 value{inst.Arg(2)};
                const IR::U1 in_range{ir.ILessThan(index, ir.Imm32(num_allocated_vgprs - reg), false)};
                const IR::U32 slot{ir.IAdd(index, ir.Imm32(reg - base))};
                ir.WriteVgprArray(IR::U32{ir.Select(in_range, slot, ir.Imm32(length))}, value);
                inst.Invalidate();
                break;
            }
            default:
                break;
            }
        }
    }
}

} // namespace Shader::Optimization
