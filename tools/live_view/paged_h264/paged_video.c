// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "paged_video.h"
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "miniz.h"
#define external_alloc(n) heap_caps_calloc(1, (n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define external_free heap_caps_free
#else
#include <zlib.h>
#define external_alloc(n) calloc(1, n)
#define external_free free
#endif
enum {
    PAGE_SIZE = 4096,
    PAGE_COUNT = 8192,
    TEXT = 0x10000,
    RAM = 0x2000000,
    END = RAM + PAGE_COUNT * PAGE_SIZE,
    CACHE_LIMIT = 512,
    ROM_BLOCK = 8192,
    ROM_SLOTS = 12,
    MAX_ROM_PAGES = 256
};
typedef struct {
    uint16_t page;
    bool used, dirty;
    uint8_t data[PAGE_SIZE];
} slot_t;
struct kvm_paged_vm {
    jmp_buf unwind;
    const uint8_t *rom;
    uint32_t rom_bytes, pc, reg[32];
    const uint8_t *packed, *offsets;
    uint8_t *rom_cache;
    uint16_t rom_page[ROM_SLOTS], rom_loaded[MAX_ROM_PAGES];
    unsigned rom_next;
    FILE *source, *scratch;
    unsigned cache_pages, next;
    uint16_t loaded[PAGE_COUNT];
    uint16_t disk_page[PAGE_COUNT];
    slot_t *slots;
    const kvm_paged_sink_t *sink;
    kvm_paged_stats_t *stats;
};
static uint32_t word(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
static void disk(kvm_paged_vm_t *v, slot_t *s, bool write)
{
    if (v->sink->stop && v->sink->stop(v->sink->ctx))
        longjmp(v->unwind, 2);
    if (!v->disk_page[s->page]) {
        v->disk_page[s->page] = (uint16_t)(v->stats->scratch_bytes / PAGE_SIZE + 1);
        v->stats->scratch_bytes += PAGE_SIZE;
    }
    if (fseek(v->scratch, (long)(v->disk_page[s->page] - 1) * PAGE_SIZE, SEEK_SET))
        longjmp(v->unwind, 3);
    size_t done;
    if (write) {
        done = fwrite(s->data, 1, PAGE_SIZE, v->scratch);
        ++v->stats->page_writes;
    } else {
        done = fread(s->data, 1, PAGE_SIZE, v->scratch);
        ++v->stats->page_reads;
    }
    if (done != PAGE_SIZE)
        longjmp(v->unwind, 3);
}
static slot_t *miss(kvm_paged_vm_t *v, unsigned pg)
{
    unsigned index = v->next;
    if (++v->next == v->cache_pages)
        v->next = 0;
    slot_t *s = v->slots + index;
    if (s->used) {
        if (s->dirty) {
            disk(v, s, true);
        }
        v->loaded[s->page] = 0;
    }
    s->page = pg;
    s->used = true;
    s->dirty = false;
    if (v->disk_page[pg])
        disk(v, s, false);
    else
        memset(s->data, 0, PAGE_SIZE);
    v->loaded[pg] = (uint16_t)(index + 1);
    return s;
}
static inline uint8_t *ram(kvm_paged_vm_t *v, uint32_t a, bool write)
{
    unsigned pg = (a - RAM) / PAGE_SIZE;
    if (pg >= PAGE_COUNT)
        longjmp(v->unwind, 4);
    unsigned index = v->loaded[pg];
    slot_t *s = index ? v->slots + index - 1 : miss(v, pg);
    if (write)
        s->dirty = true;
    return s->data + (a & (PAGE_SIZE - 1));
}
static const uint8_t *rom(kvm_paged_vm_t *v, uint32_t off)
{
    if (v->rom)
        return v->rom + off;
    unsigned pg = off / ROM_BLOCK;
    unsigned tag = word(v->offsets + 4 * pg);
    if (tag >> 31)
        return v->packed + (tag & 0x7fffffffu) + (off % ROM_BLOCK);
    unsigned resident = v->rom_loaded[pg];
    if (!resident) {
        unsigned slot = v->rom_next;
        if (++v->rom_next == ROM_SLOTS)
            v->rom_next = 0;
        if (v->rom_page[slot])
            v->rom_loaded[v->rom_page[slot] - 1] = 0;
        uint8_t *out = v->rom_cache + slot * ROM_BLOCK;
        unsigned begin = tag, end = word(v->offsets + 4 * (pg + 1)) & 0x7fffffffu;
        unsigned size = v->rom_bytes - pg * ROM_BLOCK;
        if (size > ROM_BLOCK)
            size = ROM_BLOCK;
#ifdef ESP_PLATFORM
        if (tinfl_decompress_mem_to_mem(out, size, v->packed + begin, end - begin, TINFL_FLAG_PARSE_ZLIB_HEADER) !=
            size)
            longjmp(v->unwind, 4);
#else
        unsigned long out_size = size;
        if (uncompress(out, &out_size, v->packed + begin, end - begin) != Z_OK || out_size != size)
            longjmp(v->unwind, 4);
#endif
        ++v->stats->rom_loads;
        v->rom_page[slot] = (uint16_t)(pg + 1);
        v->rom_loaded[pg] = resident = (uint16_t)(slot + 1);
    }
    return v->rom_cache + (resident - 1) * ROM_BLOCK + (off % ROM_BLOCK);
}
static bool span(kvm_paged_vm_t *v, uint32_t a, void *buf, size_t n, bool write)
{
    uint8_t *p = buf;
    if (!write && a >= TEXT && a - TEXT <= v->rom_bytes && n <= v->rom_bytes - (a - TEXT)) {
        while (n) {
            unsigned off = a - TEXT;
            size_t chunk = ROM_BLOCK - off % ROM_BLOCK;
            if (chunk > n)
                chunk = n;
            memcpy(p, rom(v, off), chunk);
            a += (uint32_t)chunk;
            p += chunk;
            n -= chunk;
        }
        return true;
    }
    if (a < RAM || a > END || n > END - a)
        return false;
    while (n) {
        size_t chunk = PAGE_SIZE - (a % PAGE_SIZE);
        if (chunk > n)
            chunk = n;
        uint8_t *ptr = ram(v, a, write);
        if (write)
            memcpy(ptr, p, chunk);
        else
            memcpy(p, ptr, chunk);
        n -= chunk;
        a += (uint32_t)chunk;
        p += chunk;
    }
    return true;
}
bool kvm_paged_read(kvm_paged_vm_t *v, uint32_t a, void *dst, size_t n)
{
    return v && dst && span(v, a, dst, n, false);
}
static inline uint32_t load(kvm_paged_vm_t *v, uint32_t a, unsigned size)
{
    uint32_t x = 0;
    if ((a & (PAGE_SIZE - 1)) + size > PAGE_SIZE) {
        if (!span(v, a, &x, size, false))
            longjmp(v->unwind, 4);
        return x;
    }
    const uint8_t *p;
    if (a >= RAM && a < END)
        p = ram(v, a, false);
    else if (a >= TEXT && a - TEXT <= v->rom_bytes && size <= v->rom_bytes - (a - TEXT))
        p = rom(v, a - TEXT);
    else
        longjmp(v->unwind, 4);
    switch (size) {
    case 4:
        return word(p);
    case 2: {
        uint16_t h;
        memcpy(&h, p, 2);
        return h;
    }
    default:
        return *p;
    }
}
static inline void store(kvm_paged_vm_t *v, uint32_t a, uint32_t val, unsigned size)
{
    if ((a & (PAGE_SIZE - 1)) + size > PAGE_SIZE) {
        if (!span(v, a, &val, size, true))
            longjmp(v->unwind, 4);
    } else {
        uint8_t *p = ram(v, a, true);
        if (size == 4)
            memcpy(p, &val, 4);
        else if (size == 2) {
            uint16_t h = (uint16_t)val;
            memcpy(p, &h, 2);
        } else
            *p = (uint8_t)val;
    }
}
static int rpc(kvm_paged_vm_t *v)
{
    uint32_t *r = v->reg;
    if (r[17] == 0) {
        v->stats->guest_status = (int32_t)r[10];
        return 1;
    }
    if (r[17] == 1 && r[11] <= 8192) {
        uint8_t block[512];
        uint32_t a = r[10], left = r[11], n = 0;
        while (left) {
            unsigned count = left < sizeof(block) ? left : sizeof(block);
            unsigned got = (unsigned)fread(block, 1, count, v->source);
            if (!span(v, a, block, got, true) || ferror(v->source))
                longjmp(v->unwind, 3);
            n += got;
            a += got;
            left -= got;
            if (got < count)
                break;
        }
        r[10] = n;
        return 0;
    }
    if (r[17] == 2 && r[11] == 40) {
        uint32_t d[10];
        if (!span(v, r[10], d, sizeof(d), false) || d[0] < 2 || d[1] < 2 || d[0] > 960 || d[1] > 544)
            longjmp(v->unwind, 4);
        kvm_paged_picture_t f = {.width = d[0], .height = d[1], .full_range = d[8] != 0, .bt709 = d[9] != 0};
        for (unsigned i = 0; i < 3; i++) {
            unsigned w = (d[0] + !!i) >> (!!i), h = (d[1] + !!i) >> (!!i);
            f.plane[i] = d[2 + i];
            f.stride[i] = d[5 + i];
            if (f.plane[i] < RAM || f.plane[i] >= END || f.stride[i] < w ||
                (uint64_t)(h - 1) * f.stride[i] + w > END - f.plane[i])
                longjmp(v->unwind, 4);
        }
        if (!v->sink->frame(v->sink->ctx, v, &f))
            longjmp(v->unwind, 2);
        ++v->stats->frames;
        r[10] = 0;
        return 0;
    }
    longjmp(v->unwind, 4);
}
static inline int32_t signext(uint32_t x, unsigned bits)
{
    return (int32_t)(x << (32 - bits)) >> (32 - bits);
}
static int execute(kvm_paged_vm_t *v)
{
    uint32_t *r = v->reg, pc = v->pc;
    for (;;) {
        if ((pc & 3) || pc < TEXT || pc - TEXT > v->rom_bytes - 4)
            longjmp(v->unwind, 4);
        uint32_t i = word(rom(v, pc - TEXT)), next = pc + 4;
        uint32_t a = r[(i >> 15) & 31], b = r[(i >> 20) & 31], value = 0;
        unsigned rd = (i >> 7) & 31, f = (i >> 12) & 7, op = i & 127;
        int32_t imm = (int32_t)i >> 20;
        v->stats->last_pc = pc;
        if ((++v->stats->steps & 65535) == 0 && v->sink->stop && v->sink->stop(v->sink->ctx))
            longjmp(v->unwind, 2);
        switch (op) {
        case 0x37:
            value = i & 0xfffff000;
            break;
        case 0x17:
            value = pc + (i & 0xfffff000);
            break;
        case 0x6f:
            value = next;
            next = pc +
                   signext(((i >> 31) << 20) | (i & 0xff000) | ((i >> 20 & 1) << 11) | ((i >> 21 & 1023) << 1), 21);
            break;
        case 0x67:
            if (f)
                longjmp(v->unwind, 4);
            value = next;
            next = (a + imm) & ~1u;
            break;
        case 0x63: {
            bool take;
            switch (f) {
            case 0:
                take = a == b;
                break;
            case 1:
                take = a != b;
                break;
            case 4:
                take = (int32_t)a < (int32_t)b;
                break;
            case 5:
                take = (int32_t)a >= (int32_t)b;
                break;
            case 6:
                take = a < b;
                break;
            case 7:
                take = a >= b;
                break;
            default:
                longjmp(v->unwind, 4);
            }
            if (take)
                next = pc + signext(((i >> 31) << 12) | ((i >> 7 & 1) << 11) | ((i >> 25 & 63) << 5) |
                                        ((i >> 8 & 15) << 1),
                                    13);
            rd = 0;
            break;
        }
        case 0x03:
            if (f > 5 || f == 3)
                longjmp(v->unwind, 4);
            value = load(v, a + imm, 1u << (f & 3));
            if (f < 2)
                value = signext(value, 8u << f);
            break;
        case 0x23:
            if (f > 2)
                longjmp(v->unwind, 4);
            store(v, a + signext(((i >> 25) << 5) | rd, 12), b, 1u << f);
            rd = 0;
            break;
        case 0x13:
        case 0x33: {
            uint32_t arg = op == 0x33 ? b : (uint32_t)imm;
            unsigned top = i >> 25;
            bool reg = op == 0x33;
            if ((reg || f == 1 || f == 5) && top != 0 && !(top == 32 && (f == 5 || (reg && f == 0))))
                longjmp(v->unwind, 4);
            switch (f) {
            case 0:
                value = reg && top == 32 ? a - arg : a + arg;
                break;
            case 1:
                value = a << (arg & 31);
                break;
            case 2:
                value = (int32_t)a < (int32_t)arg;
                break;
            case 3:
                value = a < arg;
                break;
            case 4:
                value = a ^ arg;
                break;
            case 5:
                value = top == 32 ? (uint32_t)((int32_t)a >> (arg & 31)) : a >> (arg & 31);
                break;
            case 6:
                value = a | arg;
                break;
            default:
                value = a & arg;
            }
            break;
        }
        case 0x0f:
            rd = 0;
            break;
        case 0x73:
            if (i != 0x73)
                longjmp(v->unwind, 4);
            if (rpc(v))
                return v->stats->guest_status ? 5 : 0;
            rd = 0;
            break;
        default:
            longjmp(v->unwind, 4);
        }
        if (rd)
            r[rd] = value;
        pc = next;
    }
}
static bool image(kvm_paged_vm_t *v, const uint8_t *elf, size_t n)
{
    v->reg[2] = END - 16;
    if (n >= 24 && !memcmp(elf, "KVR1", 4)) {
        unsigned size = word(elf + 4), entry = word(elf + 8), data = word(elf + 12), pg = word(elf + 16);
        if (!pg || pg > MAX_ROM_PAGES || size < 4 || size > pg * ROM_BLOCK || size <= (pg - 1) * ROM_BLOCK ||
            word(elf + 20) != ROM_BLOCK || n < 24 + 4 * (pg + 1) || data > n)
            return false;
        const uint8_t *table = elf + 24;
        if ((word(table) & 0x7fffffffu) != 24 + 4 * (pg + 1))
            return false;
        for (unsigned i = 0; i < pg; i++) {
            uint32_t tag = word(table + 4 * i), b = tag & 0x7fffffffu, e = word(table + 4 * (i + 1)) & 0x7fffffffu;
            unsigned len = (i == pg - 1) ? size - i * ROM_BLOCK : ROM_BLOCK;
            if ((tag >> 31) && e - b != len)
                return false;
            if (e <= b || e > n - data)
                return false;
        }
        if (word(table + 4 * pg) != n - data)
            return false;
        v->packed = elf;
        v->offsets = table;
        v->rom_bytes = size;
        v->pc = entry;
        v->rom_cache = external_alloc(ROM_BLOCK * ROM_SLOTS);
        if (!v->rom_cache)
            longjmp(v->unwind, 1);
        return span(v, RAM, (void *)(elf + n - data), data, true);
    }
    if (n < 52 || memcmp(elf, "\177ELF\1\1\1", 7) || word(elf + 16) != 0x00f30002)
        return false;
    unsigned ph = word(elf + 28), stride = elf[42] | elf[43] << 8, count = elf[44] | elf[45] << 8;
    if (stride != 32 || count > 8 || ph > n || count * stride > n - ph)
        return false;
    bool data = false;
    for (unsigned i = 0; i < count; i++) {
        const uint8_t *p = elf + ph + i * stride;
        if (word(p) != 1)
            continue;
        uint32_t off = word(p + 4), a = word(p + 8), sz = word(p + 16), memory = word(p + 20);
        if (off > n || sz > n - off || sz > memory)
            return false;
        if (a == TEXT && word(p + 24) == 5 && !v->rom && sz >= 4 && sz < RAM - TEXT) {
            v->rom = elf + off;
            v->rom_bytes = sz;
        } else if (a == RAM && word(p + 24) == 6 && !data && memory <= END - RAM) {
            if (!span(v, RAM, (void *)(elf + off), sz, true))
                return false;
            data = true;
        } else
            return false;
    }
    v->pc = word(elf + 24);
    v->reg[2] = END - 16;
    return v->rom && data;
}
int kvm_paged_run(const void *elf, size_t n, FILE *source, FILE *scratch, unsigned pages, bool large_bank,
                  const kvm_paged_sink_t *sink, kvm_paged_stats_t *stats)
{
    if (!elf || !source || !scratch || source == scratch || !sink || !sink->frame || !stats || pages < 2 ||
        pages > CACHE_LIMIT)
        return 1;
    memset(stats, 0, sizeof(*stats));
    kvm_paged_vm_t *v = external_alloc(sizeof(*v));
    if (!v)
        return 1;
    v->slots = external_alloc(pages * sizeof(slot_t));
    if (!v->slots) {
        external_free(v);
        return 1;
    }
    v->source = source;
    v->scratch = scratch;
    v->sink = sink;
    v->stats = stats;
    v->cache_pages = pages;
    v->reg[10] = large_bank;
    // C restricts setjmp to a controlling expression or a full expression
    // statement. Use the saved fault reason after the nonlocal return.
    int rc;
    switch (setjmp(v->unwind)) {
    case 0: rc = image(v, elf, n) ? execute(v) : 4; break;
    case 1: rc = 1; break;
    case 2: rc = 2; break;
    case 3: rc = 3; break;
    default: rc = 4; break;
    }
    external_free(v->rom_cache);
    external_free(v->slots);
    external_free(v);
    return rc;
}
