// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Reload leak test of the vendored PinyinIME (TASK-072 footprint spike). The engine heap (the renamed C calls and
// operator new) is counted through a pointer table, so memory that reaches the renamed free() from strdup() is
// tolerated. Two upstream leaks were found on board #1 (~200 KB of PSRAM per open/close) and fixed locally
// (dicttrie.cpp, spellingtrie.cpp, marked "KVMpad local change"); this test keeps them fixed: after the first
// cycle the live bytes after close must not grow over further open/close cycles.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "ime_pinyin.h"

#define CAP (1u << 18)
static void *s_p[CAP];
static size_t s_n[CAP];
static int s_kind[CAP];
static long g_live[2]; // [0] C heap, [1] operator new

static size_t slot(void *p)
{
    size_t h = ((size_t)p >> 3) * 2654435761u % CAP;
    while (s_p[h] != nullptr && s_p[h] != p) {
        h = (h + 1) % CAP;
    }
    return h;
}

static void track(void *p, size_t n, int kind)
{
    if (p == nullptr) {
        return;
    }
    const size_t h = slot(p);
    s_p[h] = p;
    s_n[h] = n;
    s_kind[h] = kind;
    g_live[kind] += (long)n;
}

static void untrack(void *p)
{
    const size_t h = slot(p);
    if (s_p[h] != p) {
        return; // not ours (e.g. strdup): tolerated
    }
    g_live[s_kind[h]] -= (long)s_n[h];
    s_p[h] = nullptr;
    // Linear probing without tombstones: re-insert the rest of the cluster.
    size_t j = (h + 1) % CAP;
    while (s_p[j] != nullptr) {
        void *q = s_p[j];
        const size_t n = s_n[j];
        const int k = s_kind[j];
        s_p[j] = nullptr;
        const size_t t = slot(q);
        s_p[t] = q;
        s_n[t] = n;
        s_kind[t] = k;
        j = (j + 1) % CAP;
    }
}

extern "C" void *ime_engine_malloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    track(p, n, 0);
    return p;
}

extern "C" void *ime_engine_calloc(size_t n, size_t s)
{
    void *p = calloc(n ? n : 1, s ? s : 1);
    track(p, n * s, 0);
    return p;
}

extern "C" void ime_engine_free(void *p)
{
    if (p == nullptr) {
        return;
    }
    untrack(p);
    free(p);
}

extern "C" void *ime_engine_realloc(void *p, size_t n)
{
    if (p != nullptr) {
        untrack(p);
    }
    void *q = realloc(p, n);
    track(q, n, 0);
    return q;
}

extern "C" void ime_alloc_enter(void)
{
}

extern "C" void ime_alloc_leave(void)
{
}

void *operator new(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    track(p, n, 1);
    return p;
}

void *operator new[](size_t n)
{
    return operator new(n);
}

void operator delete(void *p) noexcept
{
    if (p == nullptr) {
        return;
    }
    untrack(p);
    free(p);
}

void operator delete[](void *p) noexcept
{
    operator delete(p);
}

void operator delete(void *p, size_t) noexcept
{
    operator delete(p);
}

void operator delete[](void *p, size_t) noexcept
{
    operator delete(p);
}

int main(int argc, char **argv)
{
    const char *dict = argc > 1 ? argv[1] : "res/raw/dict_pinyin.dat";
    long first[2] = {0, 0};
    int failures = 0;
    for (int i = 0; i < 6; i++) {
        const bool ok = ime_pinyin_open(dict);
        const size_t n = ime_pinyin_search("nihao", 5);
        ime_pinyin_close();
        printf("  cycle %d open=%d candidates=%u: after close malloc %ld new %ld\n", i, ok, (unsigned)n, g_live[0],
               g_live[1]);
        if (!ok || n == 0) {
            failures++;
        }
        if (i == 0) {
            first[0] = g_live[0];
            first[1] = g_live[1];
        } else if (g_live[0] != first[0] || g_live[1] != first[1]) {
            printf("  FAIL: live bytes grew after cycle %d\n", i);
            failures++;
        }
    }
    printf("test_pinyin_leak: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
