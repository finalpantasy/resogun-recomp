/* Generate RGISA1 with the same decoder the renderer already links. */
#include "prepare.h"
#include "isa_rewrite.h"
#include <Zydis/Zydis.h>
#include <stdlib.h>
#include <string.h>

static unsigned kind_for(ZydisMnemonic mnemonic, ZydisOperandType first) {
    switch (mnemonic) {
    case ZYDIS_MNEMONIC_MOVBE: return first == ZYDIS_OPERAND_TYPE_REGISTER ? ISA_MOVBE_LOAD : ISA_MOVBE_STORE;
    case ZYDIS_MNEMONIC_LZCNT: return ISA_LZCNT;
    case ZYDIS_MNEMONIC_TZCNT: return ISA_TZCNT;
    case ZYDIS_MNEMONIC_ANDN: return ISA_ANDN;
    case ZYDIS_MNEMONIC_BEXTR: return ISA_BEXTR;
    case ZYDIS_MNEMONIC_BLSI: return ISA_BLSI;
    case ZYDIS_MNEMONIC_BLSMSK: return ISA_BLSMSK;
    case ZYDIS_MNEMONIC_BLSR: return ISA_BLSR;
    case ZYDIS_MNEMONIC_VCVTPH2PS: return ISA_VCVTPH2PS;
    case ZYDIS_MNEMONIC_VCVTPS2PH: return ISA_VCVTPS2PH;
    default: return 0;
    }
}

static uint8_t gpr_number(ZydisRegister reg) {
    ZydisRegisterClass cls = ZydisRegisterGetClass(reg);
    return cls >= ZYDIS_REGCLASS_GPR16 && cls <= ZYDIS_REGCLASS_GPR64 ? (uint8_t)ZydisRegisterGetId(reg) : ISA_NO_REG;
}

static IsaSite make_site(const ZydisDecodedInstruction *ins, const ZydisDecodedOperand *operands,
                         unsigned kind, const uint8_t *image, size_t at) {
    IsaSite site = {.offset = at, .length = ins->length, .kind = (uint8_t)kind,
                    .dst = ISA_NO_REG, .src1 = ISA_NO_REG, .src2 = ISA_NO_REG,
                    .base = ISA_NO_REG, .index = ISA_NO_REG};
    int unsupported = kind >= ISA_VCVTPH2PS ||
                      (ins->attributes & (ZYDIS_ATTRIB_HAS_SEGMENT | ZYDIS_ATTRIB_HAS_LOCK));
    uint8_t *regs[] = {&site.dst, &site.src1, &site.src2};
    for (unsigned i = 0; i < ins->operand_count_visible && i < 3; ++i) {
        const ZydisDecodedOperand *op = &operands[i];
        if (op->type == ZYDIS_OPERAND_TYPE_REGISTER) {
            *regs[i] = gpr_number(op->reg.value);
            if (!site.width && kind < ISA_VCVTPH2PS) site.width = (uint8_t)op->size;
            if (*regs[i] == ISA_NO_REG) unsupported = 1;
        } else if (op->type == ZYDIS_OPERAND_TYPE_MEMORY) {
            site.mem_slot = i ? ISA_MEM_SRC : ISA_MEM_DST;
            site.scale = op->mem.scale ? op->mem.scale : 1;
            site.disp = op->mem.disp.value;
            if (op->mem.base == ZYDIS_REGISTER_RIP || op->mem.base == ZYDIS_REGISTER_EIP) {
                ZyanU64 target;
                if (ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(ins, op, at, &target))) site.disp = (int64_t)target;
                else unsupported = 1;
                site.mem_flags = ISA_MEM_RIP_RELATIVE;
            } else {
                site.base = gpr_number(op->mem.base);
                site.index = gpr_number(op->mem.index);
            }
            if (ins->address_width != 64) unsupported = 1;
        }
    }
    memcpy(site.original, image + at, ins->length);
    /* Four-byte sites always trap. Moving a neighboring instruction requires global branch analysis. */
    site.mode = unsupported ? ISA_MODE_UNSUPPORTED : ins->length >= 5 ? ISA_MODE_JMP : ISA_MODE_INT3;
    return site;
}

static int site_order(const void *a, const void *b) {
    uint64_t l = ((const IsaSite *)a)->offset, r = ((const IsaSite *)b)->offset;
    return l == r ? 0 : l < r ? -1 : 1;
}

typedef struct {
    char magic[8];
    uint64_t size;
    uint8_t hash[32];
    uint32_t count, unsupported;
    uint64_t reserved;
} IsaHeader;
_Static_assert(sizeof(IsaHeader) == 64, "RGISA1 header layout");

int prepare_isa_complete(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    IsaHeader header;
    int complete = fread(&header, 1, sizeof header, file) == sizeof header &&
                   !memcmp(header.magic, "RGISA1\0", 8) && header.count <= (1u << 20) &&
                   !fseek(file, 0, SEEK_END) &&
                   ftell(file) == (long)(sizeof header + header.count * sizeof(IsaSite));
    fclose(file);
    return complete;
}

int prepare_isa_table(const char *path, const uint8_t *image, size_t size,
                      const PrepareSegment *segments, size_t segment_count) {
    ZydisDecoder decoder;
    if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64))) return 1;
    IsaSite *sites = NULL;
    size_t count = 0, capacity = 0;
    int failed = 1;
    for (size_t i = 0; i < segment_count; ++i) {
        const PrepareSegment *seg = &segments[i];
        if (!(seg->flags & 1)) continue;
        if (seg->address > size || seg->size > size - seg->address) goto done;
        size_t end = (size_t)(seg->address + seg->size);
        for (size_t at = (size_t)seg->address; at < end;) {
            ZydisDecodedInstruction ins;
            ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
            if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, image + at, end - at, &ins, ops))) {
                ++at;
                continue;
            }
            unsigned kind = ins.operand_count_visible ? kind_for(ins.mnemonic, ops[0].type) : 0;
            if (kind) {
                if (ins.length < 4 || count == (1u << 20)) goto done;
                if (count == capacity) {
                    capacity = capacity ? 2 * capacity : 256;
                    IsaSite *next = realloc(sites, capacity * sizeof *sites);
                    if (!next) goto done;
                    sites = next;
                }
                sites[count++] = make_site(&ins, ops, kind, image, at);
            }
            at += ins.length;
        }
    }

    if (count) qsort(sites, count, sizeof *sites, site_order);
    uint32_t records = 0, unsupported = 0;
    for (size_t i = 0; i < count; ++i) {
        if (i && sites[i].offset == sites[i - 1].offset) {
            if (memcmp(&sites[i], &sites[i - 1], sizeof *sites)) goto done;
            continue;
        }
        unsupported += sites[i].mode == ISA_MODE_UNSUPPORTED;
        sites[records++] = sites[i];
    }
    IsaHeader header = {"RGISA1", size, {0}, records, unsupported, 0};
    isa_sha256(image, size, header.hash);
    FILE *f = fopen(path, "wb");
    if (!f) goto done;
    failed = fwrite(&header, sizeof header, 1, f) != 1 ||
             (records && fwrite(sites, sizeof *sites, records, f) != records);
    if (fclose(f)) failed = 1;
    if (!failed) printf("ISA: %u sites, %u unsupported encodings\n", (unsigned)records, (unsigned)unsupported);
done:
    free(sites);
    return failed;
}
