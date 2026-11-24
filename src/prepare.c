/* Plaintext PS4 SELF/ELF preparation and linking. RGIMAGE7 targets little-endian x64. */
#include "prepare.h"
#include "isa_rewrite.h"
#include <dirent.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { LIMIT = 512 * 1024 * 1024, LOAD = 1, DYNAMIC = 2, TLS = 7, DYNLIB = 0x61000000,
       PARAM = 0x61000001, RELRO = 0x61000010, EH_FRAME = 0x6474e550 };
typedef struct { const uint8_t *data; uint64_t size; } Bytes;
typedef struct { uint32_t type, flags; uint64_t offset, address, filesz, memsz, align; } Segment;
typedef struct { const char *name; uint16_t version; char id[4]; } Part;
typedef struct { char nid[16]; const Part *library, *module; const char *plain; } Identity;
typedef struct {
    Bytes elf, symbols, strings, relocs[2];
    Segment *ph;
    size_t count, library_count, module_count;
    Part *libraries, *modules;
    uint64_t entry, size, init;
} Object;
typedef struct { uint64_t target, kind, value, addend; } Relocation;
typedef struct { uint64_t base, size, init, tls_address, tls_memsz, tls_filesz, tls_id, flags, param; char name[64]; } Module;
typedef struct { uint64_t index, address, kind; } Binding;
typedef struct { uint64_t index; char nid[16]; uint64_t address, kind; } ImageExport;
typedef struct { uint64_t hdr, frame, size, first, first_size; } Unwind;
typedef struct { Identity id; char name[128]; } Import;
typedef struct { Identity id; ImageExport record; } Export;
typedef struct { void *items; size_t count, capacity; } Vector;
typedef struct Block { struct Block *next; max_align_t alignment; } Block;
typedef struct { jmp_buf failure; Block *blocks; FILE *stream; const char *file; } Prep;

_Static_assert(sizeof(Module) == 136 && sizeof(Relocation) == 32 && sizeof(Binding) == 24 &&
               sizeof(ImageExport) == 40 && sizeof(Unwind) == 40, "RGIMAGE7 record layout");

static void reject(Prep *p, const char *why) {
    fprintf(stderr, "Prepare: %s: %s\n", p->file, why);
    longjmp(p->failure, 1);
}

static void *alloc(Prep *p, size_t size) {
    if (size > LIMIT) reject(p, "allocation exceeds image limit");
    Block *b = calloc(1, sizeof *b + size);
    if (!b) reject(p, "out of memory");
    b->next = p->blocks;
    p->blocks = b;
    return b + 1;
}
static void release(Prep *p) {
    if (p->stream) fclose(p->stream);
    while (p->blocks) {
        Block *b = p->blocks;
        p->blocks = b->next;
        free(b);
    }
    free(p);
}
static void push(Prep *p, Vector *v, const void *item, size_t size) {
    if (v->count == v->capacity) {
        size_t next = v->capacity ? 2 * v->capacity : 32;
        void *items = alloc(p, next * size);
        if (v->count) memcpy(items, v->items, v->count * size);
        v->items = items;
        v->capacity = next;
    }
    memcpy((uint8_t *)v->items + v->count++ * size, item, size);
}

static Bytes slice(Prep *p, Bytes b, uint64_t offset, uint64_t size) {
    if (offset > b.size || size > b.size - offset) reject(p, "table outside input");
    return (Bytes){b.data + offset, size};
}
static uint64_t word(const uint8_t *at, unsigned n) {
    uint64_t result = 0;
    while (n) result = (result << 8) | at[--n];
    return result;
}
#define U16(x) ((uint16_t)word((x), 2))
#define U32(x) ((uint32_t)word((x), 4))
#define U64(x) word((x), 8)
static const char *str_at(Prep *p, Bytes b, uint64_t at) {
    if (at >= b.size || !memchr(b.data + at, 0, (size_t)(b.size - at))) reject(p, "unterminated string");
    return (const char *)b.data + at;
}
static const char *join(Prep *p, const char *dir, const char *file) {
    char *path = alloc(p, GOLF_PATH);
    int length = snprintf(path, GOLF_PATH, "%s/%s", dir, file);
    if (length < 0 || length >= GOLF_PATH) reject(p, "path too long");
    return path;
}
static void close_stream(Prep *p) {
    FILE *file = p->stream;
    p->stream = NULL;
    if (fclose(file)) reject(p, "cannot close file");
}
static Bytes read_file(Prep *p, const char *path, long limit) {
    p->file = path;
    FILE *f = p->stream = fopen(path, "rb");
    if (!f || fseek(f, 0, SEEK_END)) reject(p, "cannot open input");
    long size = ftell(f);
    if (size <= 0 || size > limit || fseek(f, 0, SEEK_SET)) reject(p, "empty or oversized input");
    uint8_t *data = alloc(p, (size_t)size);
    if (fread(data, 1, (size_t)size, f) != (size_t)size || fgetc(f) != EOF || ferror(f))
        reject(p, "cannot read complete input");
    close_stream(p);
    return (Bytes){data, (uint64_t)size};
}
static int loaded(const Segment *s) { return s->type == LOAD || s->type == RELRO; }
static const Segment *segment(const Object *o, unsigned type) {
    for (size_t i = 0; i < o->count; ++i) if (o->ph[i].type == type) return &o->ph[i];
    return NULL;
}
static int mapped(const Object *o, uint64_t at, uint64_t size) {
    for (size_t i = 0; i < o->count; ++i) {
        const Segment *s = &o->ph[i];
        if (loaded(s) && at >= s->address && at - s->address <= s->memsz &&
            size <= s->memsz - (at - s->address)) return 1;
    }
    return 0;
}

static void read_elf(Prep *p, Bytes raw, Object *o) {
    const uint8_t *magic = slice(p, raw, 0, 4).data;
    int self = !memcmp(magic, "\x4f\x15\x3d\x1d", 4);
    unsigned blocks = self ? U16(slice(p, raw, 24, 2).data) : 0;
    uint64_t base = self ? 32 + (uint64_t)blocks * 32 : 0;
    const uint8_t *hdr = slice(p, raw, base, 64).data;
    if (memcmp(hdr, "\x7f" "ELF\2\1\1", 7) || U16(hdr + 18) != 62 || U16(hdr + 54) != 56)
        reject(p, "expected a plaintext x86-64 ELF or SELF");
    uint64_t phoff = U64(hdr + 32);
    o->entry = U64(hdr + 24);
    o->count = U16(hdr + 56);
    if (!o->count || o->count >= 256 || phoff < 64 || phoff > LIMIT - o->count * 56)
        reject(p, "invalid ELF program headers");
    const uint8_t *ph = slice(p, raw, base + phoff, o->count * 56).data;
    uint64_t header_end = phoff + o->count * 56, end = header_end;
    o->ph = alloc(p, o->count * sizeof *o->ph);
    for (size_t i = 0; i < o->count; ++i, ph += 56) {
        Segment s = {U32(ph), U32(ph + 4), U64(ph + 8), U64(ph + 16), U64(ph + 32), U64(ph + 40), U64(ph + 48)};
        if (s.offset > LIMIT || s.filesz > LIMIT - s.offset) reject(p, "file segment exceeds image limit");
        if (s.offset + s.filesz > end) end = s.offset + s.filesz;
        if (loaded(&s)) {
            if (s.filesz > s.memsz || s.address > LIMIT || s.memsz > LIMIT - s.address)
                reject(p, "invalid mapped segment");
            if (s.address + s.memsz > o->size) o->size = s.address + s.memsz;
        }
        o->ph[i] = s;
    }
    if (!o->size) reject(p, "no loadable segments");
    if (!self) { o->elf = slice(p, raw, 0, end); return; }
    uint8_t *elf = alloc(p, (size_t)end);
    memcpy(elf, slice(p, raw, base, header_end).data, (size_t)header_end);
    uint64_t (*covered)[2] = alloc(p, (blocks + 1) * sizeof *covered);
    size_t count = 1;
    covered[0][1] = header_end;
    for (unsigned i = 0; i < blocks; ++i) {
        const uint8_t *b = slice(p, raw, 32 + 32 * i, 32).data;
        uint64_t flags = U64(b), off = U64(b + 8), size = U64(b + 16);
        if (!(flags & 0x800)) continue;
        if (flags & 10) reject(p, "encrypted or compressed SELF segment");
        size_t index = (flags >> 20) & 4095;
        if (index >= o->count || size != o->ph[index].filesz || size != U64(b + 24))
            reject(p, "unsupported SELF block layout");
        uint64_t start = o->ph[index].offset;
        memcpy(elf + start, slice(p, raw, off, size).data, (size_t)size);
        covered[count][0] = start;
        covered[count++][1] = start + size;
    }
    for (size_t i = 0; i < o->count; ++i) {
        const Segment *s = &o->ph[i];
        if (!s->filesz || (!loaded(s) && s->type != DYNAMIC && s->type != DYNLIB)) continue;
        size_t j = 0;
        while (j < count && (covered[j][0] > s->offset || covered[j][1] < s->offset + s->filesz)) ++j;
        if (j == count) reject(p, "required SELF segment unavailable");
    }
    o->elf = (Bytes){elf, end};
}

static uint64_t tag(Prep *p, Bytes dynamic, uint64_t want, int required) {
    int found = 0;
    uint64_t value = 0;
    for (uint64_t i = 0; i + 16 <= dynamic.size && U64(dynamic.data + i); i += 16)
        if (U64(dynamic.data + i) == want) { found = 1; value = U64(dynamic.data + i + 8); }
    if (required && !found) reject(p, "required SCE dynamic tag missing");
    return value;
}

static void read_object(Prep *p, Bytes raw, Object *o) {
    read_elf(p, raw, o);
    const Segment *dy = segment(o, DYNAMIC), *sce = segment(o, DYNLIB);
    if (!dy || !sce || dy->filesz % 16) reject(p, "invalid SCE dynamic tables");
    Bytes dynamic = slice(p, o->elf, dy->offset, dy->filesz), blob = slice(p, o->elf, sce->offset, sce->filesz);
    o->strings = slice(p, blob, tag(p, dynamic, 0x61000035, 1), tag(p, dynamic, 0x61000037, 1));
    o->symbols = slice(p, blob, tag(p, dynamic, 0x61000039, 1), tag(p, dynamic, 0x6100003f, 1));
    o->relocs[0] = slice(p, blob, tag(p, dynamic, 0x61000029, 1), tag(p, dynamic, 0x6100002d, 1));
    o->relocs[1] = slice(p, blob, tag(p, dynamic, 0x6100002f, 1), tag(p, dynamic, 0x61000031, 1));
    if (o->symbols.size % 24 || o->relocs[0].size % 24 || o->relocs[1].size % 24)
        reject(p, "invalid symbol or relocation table size");
    o->init = tag(p, dynamic, 12, 0);
    Vector libraries = {0}, modules = {0};
    for (uint64_t i = 0; i < dynamic.size; i += 16) {
        uint64_t kind = U64(dynamic.data + i), value = U64(dynamic.data + i + 8);
        if (!kind) break;
        Vector *table;
        switch (kind) {
        case 0x61000013: case 0x61000015: table = &libraries; break;
        case 0x6100000d: case 0x6100000f: table = &modules; break;
        default: continue;
        }
        Part part = {.name = str_at(p, o->strings, (uint32_t)value), .version = (uint16_t)(value >> 32)};
        unsigned id = (unsigned)(value >> 48), n = id > 4095 ? 3 : id > 63 ? 2 : 1;
        const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
        while (n) { part.id[--n] = alphabet[id & 63]; id >>= 6; }
        const Part *parts = table->items;
        for (size_t j = 0; j < table->count; ++j)
            if (!strcmp(part.id, parts[j].id) && (part.version != parts[j].version || strcmp(part.name, parts[j].name)))
                reject(p, "conflicting SCE identity tags");
        push(p, table, &part, sizeof part);
    }
    o->libraries = libraries.items;
    o->library_count = libraries.count;
    o->modules = modules.items;
    o->module_count = modules.count;
}

static const Part *lookup_part(Prep *p, const Part *parts, size_t count, const char *id, size_t length) {
    for (size_t i = 0; i < count; ++i)
        if (strlen(parts[i].id) == length && !memcmp(parts[i].id, id, length)) return &parts[i];
    reject(p, "unrecognized SCE identity");
    return NULL;
}
static Identity identity(Prep *p, const Object *o, const uint8_t *symbol) {
    const char *name = str_at(p, o->strings, U32(symbol));
    const char *a = strchr(name, '#'), *b = a ? strchr(a + 1, '#') : NULL;
    Identity id = {.plain = name};
    if (!b) return id;
    if (a - name >= (ptrdiff_t)sizeof id.nid || strchr(b + 1, '#')) reject(p, "invalid SCE symbol name");
    memcpy(id.nid, name, (size_t)(a - name));
    id.library = lookup_part(p, o->libraries, o->library_count, a + 1, (size_t)(b - a - 1));
    id.module = lookup_part(p, o->modules, o->module_count, b + 1, strlen(b + 1));
    return id;
}
static int same_identity(const Identity *a, const Identity *b) {
    if (!a->library || !b->library) return !a->library && !b->library && !strcmp(a->plain, b->plain);
    return a->library->version == b->library->version && a->module->version == b->module->version &&
           !strcmp(a->nid, b->nid) && !strcmp(a->library->name, b->library->name) &&
           !strcmp(a->module->name, b->module->name);
}
static size_t import_id(Prep *p, Vector *imports, Identity id, int reuse) {
    if (reuse) {
        const Import *list = imports->items;
        for (size_t i = imports->count; i; --i) if (same_identity(&list[i - 1].id, &id)) return i - 1;
    }
    Import item = {.id = id};
    int n = id.library ? snprintf(item.name, sizeof item.name, "%s#%s", id.nid, id.library->name)
                       : snprintf(item.name, sizeof item.name, "%s", id.plain);
    if (n < 0 || (size_t)n >= sizeof item.name) reject(p, "import name is too long");
    push(p, imports, &item, sizeof item);
    return imports->count - 1;
}
static void map_segments(Prep *p, const Object *o, uint8_t *image, uint64_t base, Vector *segments) {
    static const uint8_t fs_load[] = {0x64, 0x48, 0x8b, 0x04, 0x25, 0, 0, 0, 0};
    for (size_t i = 0; i < o->count; ++i) {
        const Segment *s = &o->ph[i];
        if (!loaded(s)) continue;
        uint8_t *data = image + base + s->address;
        memcpy(data, slice(p, o->elf, s->offset, s->filesz).data, (size_t)s->filesz);
        PrepareSegment entry = {base + s->address, s->memsz, s->flags};
        push(p, segments, &entry, sizeof entry);
    }
    /* The guest's TLS uses GS while the host libc retains FS. */
    for (size_t i = 0; i < o->count; ++i) {
        const Segment *s = &o->ph[i];
        if (s->type != LOAD || !(s->flags & 1)) continue;
        uint8_t *code = image + base + s->address;
        for (uint64_t j = 0; j + sizeof fs_load <= s->filesz; ++j)
            if (!memcmp(code + j, fs_load, sizeof fs_load)) code[j] = 0x65;
    }
}

static void relocate(Prep *p, const Object *o, uint8_t *image, uint64_t base, uint64_t tls,
                     Vector *imports, Vector *relocs) {
    size_t count = (size_t)o->symbols.size / 24;
    size_t *indices = alloc(p, count * sizeof *indices);
    for (size_t i = 0; i < count; ++i) indices[i] = SIZE_MAX;
    for (unsigned table = 0; table < 2; ++table) {
        Bytes data = o->relocs[table];
        for (uint64_t j = 0; j < data.size; j += 24) {
            const uint8_t *r = data.data + j;
            uint64_t at = U64(r), addend = U64(r + 16);
            unsigned kind = U32(r + 8), index = U32(r + 12);
            if (!mapped(o, at, 8)) reject(p, "relocation outside mapped segments");
            Relocation result = {.target = base + at, .value = base + addend};
            if (kind != 8) {
                if (index >= count) reject(p, "invalid relocation symbol index");
                const uint8_t *s = o->symbols.data + (uint64_t)index * 24;
                unsigned type = s[4] & 15, binding = s[4] >> 4, section = U16(s + 6);
                if (kind == 16 && base && !U32(s) && type == 3 && !addend && tls) {
                    memcpy(image + result.target, &tls, 8);
                    continue;
                }
                if (kind != 1 && kind != 6 && kind != 7) reject(p, "unsupported relocation");
                if (section == 0xfff1) {
                    uint64_t value = U64(s + 8) + addend;
                    memcpy(image + result.target, &value, 8);
                    continue;
                }
                if (section) result.value += U64(s + 8);
                else {
                    Identity id = identity(p, o, s);
                    if (base && !id.library && binding == 2) {
                        memset(image + result.target, 0, 8);
                        continue;
                    }
                    result.kind = type == 1 ? 2 : 1;
                    if ((result.kind == 1 && addend) || addend >= 4096) reject(p, "unsupported import addend");
                    if (indices[index] == SIZE_MAX) indices[index] = import_id(p, imports, id, base != 0);
                    result.value = indices[index];
                    result.addend = addend;
                }
            }
            push(p, relocs, &result, sizeof result);
        }
    }
}

static int collect_exports(Prep *p, const Object *o, uint64_t base, size_t index, Vector *exports) {
    int init_env_ret = 0;
    for (uint64_t i = 0; i < o->symbols.size; i += 24) {
        const uint8_t *s = o->symbols.data + i;
        unsigned type = s[4] & 15, binding = s[4] >> 4;
        if (!U16(s + 6) || (binding != 1 && binding != 2) || (type != 1 && type != 2)) continue;
        Identity id = identity(p, o, s);
        if (!id.library) continue;
        uint64_t at = U64(s + 8), size = U64(s + 16);
        if (!mapped(o, at, size ? size : 1)) reject(p, "export outside mapped segments");
        const Export *list = exports->items;
        for (size_t j = 0; j < exports->count; ++j)
            if (same_identity(&list[j].id, &id)) reject(p, "ambiguous export identity");
        Export item = {.id = id, .record = {.index = index, .address = base + at, .kind = type == 1 ? 2 : 1}};
        memcpy(item.record.nid, id.nid, sizeof item.record.nid);
        push(p, exports, &item, sizeof item);
        /* _init_env: set the capability only for a one-byte RET in libc's executable segment. */
        if (!index && size == 1 && type == 2 && !strcmp(id.nid, "bzQExy189ZI"))
            for (size_t n = 0; n < o->count; ++n) {
                const Segment *code = &o->ph[n];
                if (code->type == LOAD && (code->flags & 1) && at >= code->address && at - code->address < code->filesz)
                    init_env_ret = slice(p, o->elf, code->offset + at - code->address, 1).data[0] == 0xc3;
            }
    }
    return init_env_ret;
}

static void bind(Prep *p, Vector *bindings, Vector imports, Vector exports) {
    const Import *names = imports.items;
    const Export *available = exports.items;
    for (size_t i = 0; i < imports.count; ++i) {
        const Identity *id = &names[i].id;
        const Export *match = NULL, *fallback = NULL;
        unsigned candidates = 0;
        for (size_t j = 0; j < exports.count; ++j) {
            const Export *entry = &available[j];
            if (same_identity(id, &entry->id)) { match = entry; break; }
            if (id->library && !strcmp(id->library->name, "libSceLibcInternal") &&
                !strcmp(entry->id.library->name, "libc") && !strcmp(entry->id.nid, id->nid)) {
                fallback = entry;
                ++candidates;
            }
        }
        if (!match && candidates == 1) match = fallback;
        if (match) push(p, bindings, &(Binding){i, match->record.address, match->record.kind}, sizeof(Binding));
    }
}

static Unwind unwind(Prep *p, const Object *o, uint64_t base) {
    Unwind out = {0};
    for (size_t i = 0; i < o->count; ++i) {
        const Segment *s = &o->ph[i];
        if (loaded(s)) { out.first = base + s->address; out.first_size = s->memsz; break; }
    }
    const Segment *hdr = segment(o, EH_FRAME);
    if (!hdr) return out;
    const uint8_t *data = slice(p, o->elf, hdr->offset, 8).data;
    if (hdr->filesz < 8 || hdr->address > o->size || data[0] != 1 || (data[1] != 0x1b && data[1] != 3))
        reject(p, "unsupported unwind header");
    int64_t frame = data[1] == 0x1b ? (int64_t)hdr->address + 4 + (int32_t)U32(data + 4) : (int64_t)U32(data + 4);
    if (frame < 0 || (uint64_t)frame >= o->size) reject(p, "unwind frame outside image");
    out.hdr = base + hdr->address;
    out.frame = base + (uint64_t)frame;
    out.size = hdr->address > (uint64_t)frame ? hdr->address - (uint64_t)frame : o->size - hdr->address;
    return out;
}

static void emit(Prep *p, const void *data, size_t size) {
    if (size && fwrite(data, 1, size, p->stream) != size) reject(p, "cannot write output");
}
static void scalar(Prep *p, uint64_t value) { emit(p, &value, sizeof value); }
static void start_output(Prep *p, const char *dir, const char *name) {
    p->file = join(p, dir, name);
    p->stream = fopen(p->file, "wb");
    if (!p->stream) reject(p, "cannot create output");
}

typedef struct { Vector modules, bindings, exports, unwind, segments, imports, relocs; } Link;
/* Check the length of a counted table before seeking past it. */
static int skip_table(FILE *file, long *remaining, size_t record_size) {
    uint8_t count[8];
    if (*remaining < (long)sizeof count || fread(count, 1, sizeof count, file) != sizeof count) return 0;
    *remaining -= (long)sizeof count;
    uint64_t rows = U64(count);
    if (rows > (uint64_t)*remaining / record_size) return 0;
    long bytes = (long)(rows * record_size);
    *remaining -= bytes;
    return !fseek(file, bytes, SEEK_CUR);
}

int prepare_boot_complete(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    int complete = 0;
    /* RGIMAGE7 puts four counted tables between the fixed header and image records. */
    uint8_t header[96];
    if (fseek(file, 0, SEEK_END)) goto done;
    long length = ftell(file);
    if (length < (long)sizeof header || fseek(file, 0, SEEK_SET) ||
        fread(header, 1, sizeof header, file) != sizeof header ||
        memcmp(header, "RGIMAGE7", 8)) goto done;
    uint64_t image = U64(header + 8), segments = U64(header + 24);
    uint64_t relocs = U64(header + 32), imports = U64(header + 40);
    if (!image || image > LIMIT || !segments || segments > 64 || relocs > 1000000 || imports > 100000)
        goto done;
    uint64_t tail = image + segments * sizeof(PrepareSegment) + relocs * sizeof(Relocation) +
                    imports * sizeof(((Import *)0)->name);
    long remaining = length - (long)sizeof header;
    if (tail > (uint64_t)remaining) goto done;
    remaining -= (long)tail;
    complete = skip_table(file, &remaining, sizeof(Module)) &&
               skip_table(file, &remaining, sizeof(Binding)) &&
               skip_table(file, &remaining, sizeof(ImageExport)) &&
               skip_table(file, &remaining, sizeof(Unwind)) && !remaining;
done:
    fclose(file);
    return complete;
}

static void write_image(Prep *p, const char *dir, const Object *main, const Link *link,
                        const uint8_t *image, uint64_t size, uint64_t flags) {
    const Segment *param = segment(main, PARAM), *tls = segment(main, TLS);
    if (!param || !mapped(main, param->address, 24)) reject(p, "missing or invalid EBOOT procparam");
    uint64_t tls_words[4] = {0};
    if (tls) {
        if (tls->filesz > tls->memsz || tls->memsz > 1024 * 1024 || !mapped(main, tls->address, tls->filesz))
            reject(p, "invalid EBOOT TLS layout");
        tls_words[0] = tls->address;
        tls_words[1] = tls->filesz;
        tls_words[2] = tls->memsz;
        tls_words[3] = tls->align;
    }

    start_output(p, dir, "boot-linked.bin");
    emit(p, "RGIMAGE7", 8);
    scalar(p, size);
    scalar(p, main->entry);
    scalar(p, link->segments.count);
    scalar(p, link->relocs.count);
    scalar(p, link->imports.count);
    scalar(p, flags);
    scalar(p, param->address);
    emit(p, tls_words, sizeof tls_words);
    scalar(p, link->modules.count);
    emit(p, link->modules.items, link->modules.count * sizeof(Module));
    scalar(p, link->bindings.count);
    emit(p, link->bindings.items, link->bindings.count * sizeof(Binding));
    scalar(p, link->exports.count);
    const Export *exports = link->exports.items;
    for (size_t i = 0; i < link->exports.count; ++i) emit(p, &exports[i].record, sizeof(ImageExport));
    scalar(p, link->unwind.count);
    emit(p, link->unwind.items, link->unwind.count * sizeof(Unwind));
    emit(p, link->segments.items, link->segments.count * sizeof(PrepareSegment));
    const Import *imports = link->imports.items;
    for (size_t i = 0; i < link->imports.count; ++i) emit(p, imports[i].name, sizeof imports[i].name);
    emit(p, link->relocs.items, link->relocs.count * sizeof(Relocation));
    emit(p, image, (size_t)size);
    close_stream(p);
}

static void read_profile(Prep *p, Bytes sfo, uint32_t values[5], int any_build) {
    if (memcmp(slice(p, sfo, 0, 20).data, "\0PSF", 4)) reject(p, "invalid SFO signature");
    uint32_t keys = U32(sfo.data + 8), data = U32(sfo.data + 12), count = U32(sfo.data + 16);
    Bytes entries = slice(p, sfo, 20, (uint64_t)count * 16);
    if (data < keys || keys < 20 + entries.size) reject(p, "invalid SFO tables");
    Bytes key_table = slice(p, sfo, keys, data - keys);
    const char *title = "", *version = "";
    values[0] = 3;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *item = entries.data + 16 * i;
        const char *key = str_at(p, key_table, U16(item));
        uint32_t size = U32(item + 4);
        if (size > U32(item + 8)) reject(p, "invalid SFO value size");
        Bytes value = slice(p, sfo, (uint64_t)data + U32(item + 12), size);
        if (!strcmp(key, "TITLE_ID")) title = str_at(p, value, 0);
        if (!strcmp(key, "APP_VER")) version = str_at(p, value, 0);
        if (strlen(key) == 20 && !memcmp(key, "USER_DEFINED_PARAM_", 19) && key[19] >= '1' && key[19] <= '4') {
            if (U16(item + 2) != 0x404 || size != 4) reject(p, "invalid SFO user parameter");
            values[key[19] - '0'] = U32(value.data);
        }
    }
    if (!any_build && (strcmp(title, "CUSA04687") || strcmp(version, "01.17")))
        reject(p, "expected CUSA04687 update 1.17");
}

typedef struct { char name[64]; } ModuleName;
typedef struct {
    Bytes main, modules[64];
    ModuleName names[64];
    size_t count;
    uint32_t profile[5];
    char key[GOLF_SOURCE_KEY];
} Source;

static int compare_module(const void *a, const void *b) {
    return strcmp(((const ModuleName *)a)->name, ((const ModuleName *)b)->name);
}
static void hex_digest(const uint8_t bytes[32], char out[GOLF_SOURCE_KEY]) {
    for (unsigned i = 0; i < 32; ++i) snprintf(out + 2 * i, 3, "%02x", bytes[i]);
}

static void read_source(Prep *p, const char *game, Source *source, int any_build) {
    static const char expected[] = "59d9dc8df97c2b39315afed269d33e63839d594c9f3357b8784ec0db7606e820";
    uint8_t hashes[68 + 64 * 96] = {'G', 'O', 'L', 1}, digest[32];
    source->main = read_file(p, join(p, game, "eboot.bin"), LIMIT);
    isa_sha256(source->main.data, source->main.size, hashes + 4);
    hex_digest(hashes + 4, source->key);
    if (!any_build && strcmp(source->key, expected))
        reject(p, "EBOOT hash differs from the pinned 1.17 dump (use --any-build to inspect it)");
    Bytes sfo = read_file(p, join(p, game, "sce_sys/param.sfo"), 1 << 20);
    read_profile(p, sfo, source->profile, any_build);
    isa_sha256(sfo.data, sfo.size, hashes + 36);

    const char *dir = p->file = join(p, game, "sce_module");
    source->names[0] = (ModuleName){"libc.prx"};
    source->names[1] = (ModuleName){"libSceFios2.prx"};
    source->count = 2;
    DIR *d = opendir(dir);
    if (!d) reject(p, "cannot open module directory");
    int invalid = 0;
    for (struct dirent *e; (e = readdir(d));) {
        const char *name = e->d_name;
        size_t n = strlen(name);
        if (n < 5 || strcmp(name + n - 4, ".prx") || !strcmp(name, source->names[0].name) ||
            !strcmp(name, source->names[1].name)) continue;
        if (n >= sizeof source->names[0].name || source->count == 64) { invalid = 1; break; }
        memcpy(source->names[source->count++].name, name, n + 1);
    }
    closedir(d);
    if (invalid) reject(p, "module count or name exceeds loader limits");
    qsort(source->names + 2, source->count - 2, sizeof(ModuleName), compare_module);

    /* Every input and the preparation format revision contribute to cache identity. */
    if (source->main.size + sfo.size >= LIMIT) reject(p, "dump input exceeds preparation limit");
    long remaining = (long)(LIMIT - source->main.size - sfo.size);
    for (size_t i = 0; i < source->count; ++i) {
        Bytes prx = read_file(p, join(p, dir, source->names[i].name), remaining);
        remaining -= (long)prx.size;
        source->modules[i] = prx;
        memcpy(hashes + 68 + i * 96, source->names[i].name, 64);
        isa_sha256(prx.data, prx.size, hashes + 68 + i * 96 + 64);
    }
    isa_sha256(hashes, 68 + source->count * 96, digest);
    hex_digest(digest, source->key);
}

int prepare_source_key(const char *game, char key[GOLF_SOURCE_KEY], int any_build) {
    Prep *p = calloc(1, sizeof *p);
    if (!p) return 1;
    if (setjmp(p->failure)) { release(p); return 1; }
    Source source = {0};
    read_source(p, game, &source, any_build);
    memcpy(key, source.key, sizeof source.key);
    release(p);
    return 0;
}

/* The plain ELF of eboot.bin (SELF header removed), as eboot.elf: the online title server reads its
   tables from it (tools/server/host.py --eboot). Written to a .part file first, then renamed. */
static void write_server_elf(Prep *p, const Object *main, const char *dir) {
    const char *part = join(p, dir, "eboot.elf.part"), *final = join(p, dir, "eboot.elf");
    start_output(p, dir, "eboot.elf.part");
    emit(p, main->elf.data, (size_t)main->elf.size);
    close_stream(p);
    remove(final);
    p->file = final;
    if (rename(part, final)) { remove(part); reject(p, "cannot write eboot.elf"); }
}

static void link_source(Prep *p, const Source *source, const char *dir) {
    Object main = {0}, *objects = alloc(p, source->count * sizeof *objects);
    p->file = "eboot.bin";
    read_object(p, source->main, &main);
    if (!mapped(&main, main.entry, 1)) reject(p, "EBOOT entry outside image");
    uint64_t size = main.size;
    for (size_t i = 0; i < source->count; ++i) {
        p->file = source->names[i].name;
        read_object(p, source->modules[i], &objects[i]);
        size = (size + 65535) & ~(uint64_t)65535;
        if (objects[i].size > LIMIT - size) reject(p, "linked image exceeds loader limit");
        size += objects[i].size;
    }

    Link link = {0};
    uint8_t *image = alloc(p, (size_t)size);
    p->file = "eboot.bin";
    map_segments(p, &main, image, 0, &link.segments);
    relocate(p, &main, image, 0, 0, &link.imports, &link.relocs);
    uint64_t base = main.size, tls_id = 2, flags = 0;
    for (size_t i = 0; i < source->count; ++i) {
        p->file = source->names[i].name;
        const Object *o = &objects[i];
        const Segment *param = segment(o, PARAM), *tls = segment(o, TLS);
        base = (base + 65535) & ~(uint64_t)65535;
        if (param && !mapped(o, param->address, 24)) reject(p, "module procparam outside image");
        if (o->init && !mapped(o, o->init, 1)) reject(p, "module initializer outside image");
        Module module = {.base = base, .size = o->size, .init = base + o->init,
                         .flags = i >= 2, .param = param ? base + param->address : 0};
        memcpy(module.name, source->names[i].name, sizeof module.name);
        if (tls) {
            if (tls->filesz > tls->memsz || !mapped(o, tls->address, tls->memsz))
                reject(p, "invalid module TLS layout");
            module.tls_address = base + tls->address;
            module.tls_memsz = tls->memsz;
            module.tls_filesz = tls->filesz;
            module.tls_id = tls_id++;
        }
        map_segments(p, o, image, base, &link.segments);
        relocate(p, o, image, base, module.tls_id, &link.imports, &link.relocs);
        if (collect_exports(p, o, base, i, &link.exports)) flags |= 1;
        push(p, &link.modules, &module, sizeof module);
        Unwind info = unwind(p, o, base);
        push(p, &link.unwind, &info, sizeof info);
        base += o->size;
    }
    p->file = "eboot.bin";
    Unwind main_info = unwind(p, &main, 0);
    push(p, &link.unwind, &main_info, sizeof main_info);
    bind(p, &link.bindings, link.imports, link.exports);
    write_image(p, dir, &main, &link, image, size, flags);
    write_server_elf(p, &main, dir);
    start_output(p, dir, "content.bin");
    emit(p, "RGCONT01", 8);
    emit(p, source->profile, sizeof source->profile);
    close_stream(p);
    p->file = join(p, dir, "isa-rewrite.bin");
    if (prepare_isa_table(p->file, image, (size_t)size, link.segments.items, link.segments.count))
        reject(p, "cannot write ISA table");
    printf("Prepared %zu modules, %zu imports, %zu native bindings\n",
           link.modules.count, link.imports.count, link.bindings.count);
}

int prepare_image(const char *game, const char *destination, const char *key, int any_build) {
    Prep *p = calloc(1, sizeof *p);
    if (!p) return 1;
    if (setjmp(p->failure)) { release(p); return 1; }
    Source source = {0};
    read_source(p, game, &source, any_build);
    if (strcmp(source.key, key)) reject(p, "dump changed during preparation");
    link_source(p, &source, destination);
    release(p);
    return 0;
}

/* An image prepared before eboot.elf was part of it gets the file without being prepared again. */
int prepare_server_elf(const char *game, const char *destination) {
    Prep *p = calloc(1, sizeof *p);
    if (!p) return 1;
    if (setjmp(p->failure)) { release(p); return 1; }
    Object main = {0};
    p->file = "eboot.bin";
    read_elf(p, read_file(p, join(p, game, "eboot.bin"), LIMIT), &main);
    write_server_elf(p, &main, destination);
    release(p);
    return 0;
}
