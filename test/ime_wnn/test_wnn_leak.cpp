// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Heap regression for ime_wnn (TECH-080, "no heap growth per keystroke"). This file replaces
// ime_alloc.cpp with a counting allocator, so it measures exactly what the engine takes: the work
// area at open, nothing per keystroke, and nothing left behind after close.
//
// The vendored OpenWnn engine itself never allocates (unlike PinyinIME it works entirely inside the
// NJ_CLASS / NJ_DIC_SET the caller owns), so the only allocation expected here is ime_wnn's own work
// area. The test asserts that, which is also what catches a future vendored change that starts
// calling malloc().
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "ime_wnn.h"

static size_t s_live;
static size_t s_calls;
static size_t s_peak;

extern "C" {
void *ime_engine_malloc(size_t n);
void *ime_engine_calloc(size_t n, size_t size);
void *ime_engine_realloc(void *p, size_t n);
void ime_engine_free(void *p);
void ime_alloc_enter(void);
void ime_alloc_leave(void);
}

extern "C" void *ime_engine_malloc(size_t n)
{
    void *p = std::malloc(n + 16);
    if (p == nullptr) {
        return nullptr;
    }
    *(size_t *)p = n;
    s_live += n;
    s_calls++;
    if (s_live > s_peak) {
        s_peak = s_live;
    }
    return (char *)p + 16;
}

extern "C" void *ime_engine_calloc(size_t n, size_t size)
{
    void *p = ime_engine_malloc(n * size);
    if (p != nullptr) {
        std::memset(p, 0, n * size);
    }
    return p;
}

extern "C" void *ime_engine_realloc(void *p, size_t n)
{
    if (p == nullptr) {
        return ime_engine_malloc(n);
    }
    const size_t old = *(size_t *)((char *)p - 16);
    void *q = ime_engine_malloc(n);
    if (q != nullptr) {
        std::memcpy(q, p, old < n ? old : n);
    }
    ime_engine_free(p);
    return q;
}

extern "C" void ime_engine_free(void *p)
{
    if (p == nullptr) {
        return;
    }
    char *base = (char *)p - 16;
    s_live -= *(size_t *)base;
    std::free(base);
}

extern "C" void ime_alloc_enter(void) {}
extern "C" void ime_alloc_leave(void) {}

static int failures;

static void check(bool ok, const char *what)
{
    if (!ok) {
        failures++;
        std::printf("FAIL: %s\n", what);
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <wnn_ja.bin>\n", argv[0]);
        return EXIT_FAILURE;
    }
    FILE *f = std::fopen(argv[1], "rb");
    if (f == nullptr) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return EXIT_FAILURE;
    }
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    void *blob = std::malloc((size_t)n);
    if (std::fread(blob, 1, (size_t)n, f) != (size_t)n) {
        return EXIT_FAILURE;
    }
    std::fclose(f);

    ime_wnn_dicts_t d;
    check(ime_wnn_dicts_from_blob(blob, (size_t)n, &d), "blob parses");

    static const char *const vectors[] = {"nihongo", "kyouhaiitenkidesu", "nihongonyuuryoku",
                                          "watashihanihongogasukidesu", "tabetai"};
    for (int cycle = 0; cycle < 6; cycle++) {
        check(ime_wnn_open(&d), "open");
        const size_t after_open = s_live;
        const size_t calls_open = s_calls;
        check(after_open == ime_wnn_work_size(), "one work area, nothing else");

        // Type each vector one letter at a time: the live heap must not move between keystrokes.
        for (const char *v : vectors) {
            const size_t len = std::strlen(v);
            for (size_t k = 1; k <= len; k++) {
                ime_wnn_search(v, k);
                uint16_t buf[256];
                const size_t cands = k;  // just exercise a few ids
                for (size_t i = 0; i < cands && i < 4; i++) {
                    ime_wnn_candidate(i, buf, 256);
                }
                if (s_live != after_open) {
                    check(false, "heap grew while typing");
                    break;
                }
            }
            // and not while fixing clauses either
            size_t guard = 0;
            size_t c = ime_wnn_search(v, len);
            while (c > 0 && !ime_wnn_is_complete() && guard++ < 32) {
                c = ime_wnn_choose(c > 1 ? 1u : 0u);
            }
            ime_wnn_cancel_last();
            check(s_live == after_open, "heap grew while choosing");
        }
        check(s_calls == calls_open, "no allocation after open");
        ime_wnn_close();
        check(s_live == 0, "nothing left after close");
        std::printf("cycle %d: work area %zu B, %zu allocations, live after close %zu B\n", cycle,
                    after_open, calls_open, s_live);
    }
    std::printf("peak live %zu B, %zu allocations in total\n", s_peak, s_calls);
    std::free(blob);
    std::printf("test_wnn_leak: %d failures\n", failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
