// resogun: ReadLaneEliminationPass on hand-built IR (SGPR spills through v_writelane/v_readlane).
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include "common/object_pool.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/program.h"

using namespace Shader;

namespace {

struct Fixture {
    Common::ObjectPool<IR::Inst> insts;
    Common::ObjectPool<IR::Block> blocks;
    Info info{};
    IR::Program program{info};

    IR::Block* NewBlock() {
        IR::Block* const block = blocks.Create(insts);
        program.blocks.push_back(block);
        return block;
    }
};

int Count(IR::Program& program, IR::Opcode op) {
    int n = 0;
    for (IR::Block* const block : program.blocks) {
        for (const IR::Inst& inst : block->Instructions()) {
            n += inst.GetOpcode() == op;
        }
    }
    return n;
}

// The value the first use of `read`'s result now sees.
IR::Value UseOf(IR::Inst* user) {
    return user->Arg(0);
}

// Straight line: the reload gives the spilled value; another lane looks through to the VGPR's
// previous value, which is a vector value and keeps a real lane read.
void StraightLine() {
    Fixture f;
    IR::Block* const block = f.NewBlock();
    IR::IREmitter ir{*block};
    const IR::U32 vector = ir.LaneId();
    const IR::U32 spilled = ir.GetUserData(IR::ScalarReg::S4);
    const IR::U32 vgpr = ir.WriteLane(vector, spilled, ir.Imm32(5));
    const IR::U32 same = ir.ReadLane(vgpr, ir.Imm32(5));
    const IR::U32 other = ir.ReadLane(vgpr, ir.Imm32(3));
    IR::Inst* const use_same = ir.BitwiseNot(same).Inst();
    IR::Inst* const use_other = ir.BitwiseNot(other).Inst();
    Optimization::ReadLaneEliminationPass(f.program);
    assert(UseOf(use_same) == IR::Value{spilled});
    IR::Inst* const read = UseOf(use_other).Inst();
    assert(read->GetOpcode() == IR::Opcode::ReadLane);
    assert(read->Arg(0) == IR::Value{vector});
    assert(read->Arg(1).U32() == 3);
    // The v_writelane that is left (it feeds nothing now) is lowered, not emitted as 0.
    assert(Count(f.program, IR::Opcode::WriteLane) == 0);
}

// Diamond: lane 2 holds a spill on one path, an unrelated vector value on the other. The reload
// after the merge becomes a phi of the spilled value and a lane read of the other path's value,
// placed in that path's block.
void Diamond() {
    Fixture f;
    IR::Block* const top = f.NewBlock();
    IR::Block* const left = f.NewBlock();
    IR::Block* const right = f.NewBlock();
    IR::Block* const merge = f.NewBlock();
    top->AddBranch(left);
    top->AddBranch(right);
    left->AddBranch(merge);
    right->AddBranch(merge);
    const IR::U32 base = IR::IREmitter{*top}.LaneId();
    IR::IREmitter l{*left};
    const IR::U32 spilled = l.GetUserData(IR::ScalarReg::S6);
    const IR::U32 parked = l.WriteLane(base, spilled, l.Imm32(2));
    IR::IREmitter r{*right};
    const IR::U32 unrelated{r.IAdd(base, r.Imm32(7))};
    IR::IREmitter m{*merge};
    IR::Inst* const phi = &*merge->PrependNewInst(merge->begin(), IR::Opcode::Phi);
    phi->SetFlags(IR::Type::U32);
    phi->AddPhiOperand(left, parked);
    phi->AddPhiOperand(right, unrelated);
    const IR::U32 reload = m.ReadLane(IR::U32{IR::Value{phi}}, m.Imm32(2));
    IR::Inst* const use = m.BitwiseNot(reload).Inst();
    Optimization::ReadLaneEliminationPass(f.program);
    IR::Inst* const lane_phi = UseOf(use).Inst();
    assert(lane_phi->GetOpcode() == IR::Opcode::Phi && lane_phi != phi);
    bool saw_spill = false, saw_read = false;
    for (size_t i = 0; i < lane_phi->NumArgs(); ++i) {
        const IR::Value arg = lane_phi->Arg(i);
        if (lane_phi->PhiBlock(i) == left) {
            saw_spill = arg == IR::Value{spilled};
        } else {
            IR::Inst* const read = arg.Inst();
            saw_read = read->GetOpcode() == IR::Opcode::ReadLane &&
                       read->Arg(0) == IR::Value{unrelated} && read->GetParent() == right;
        }
    }
    assert(saw_spill && saw_read);
}

// Loop: the spill is made before the loop, the VGPR goes around it unchanged in lane 1 (another
// lane is rewritten inside), and the reload inside the loop folds to the spilled value.
void Loop() {
    Fixture f;
    IR::Block* const entry = f.NewBlock();
    IR::Block* const header = f.NewBlock();
    IR::Block* const exit = f.NewBlock();
    entry->AddBranch(header);
    header->AddBranch(header);
    header->AddBranch(exit);
    IR::IREmitter e{*entry};
    const IR::U32 spilled = e.GetUserData(IR::ScalarReg::S8);
    const IR::U32 parked = e.WriteLane(e.LaneId(), spilled, e.Imm32(1));
    IR::Inst* const phi = &*header->PrependNewInst(header->begin(), IR::Opcode::Phi);
    phi->SetFlags(IR::Type::U32);
    IR::IREmitter h{*header};
    const IR::U32 reload = h.ReadLane(IR::U32{IR::Value{phi}}, h.Imm32(1));
    IR::Inst* const use = h.BitwiseNot(reload).Inst();
    const IR::U32 again = h.WriteLane(IR::U32{IR::Value{phi}}, h.Imm32(9u), h.Imm32(4));
    phi->AddPhiOperand(entry, parked);
    phi->AddPhiOperand(header, again);
    Optimization::ReadLaneEliminationPass(f.program);
    assert(UseOf(use) == IR::Value{spilled});
}

} // namespace

int main() {
    StraightLine();
    Diamond();
    Loop();
    std::puts("lane pass: 3 cases passed");
    return 0;
}
