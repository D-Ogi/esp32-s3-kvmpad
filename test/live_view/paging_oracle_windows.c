// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
// Windows-only I/O oracle. VirtualProtect supplies the traps which the ESP needs
// as explicit software accesses. All counters are application-level page I/O.
#include <windows.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PAGE 4096
#define PAGES 8192
#define MAX_RESIDENT 512
typedef struct {
    uint16_t run;
    unsigned used : 1, stored : 1, loaded : 1, dirty : 1;
    size_t size;
} page_t;
static page_t map[PAGES];
static unsigned char *base;
static HANDLE file;
static unsigned resident[MAX_RESIDENT], capacity, count, hand;
static unsigned long long reads, writes, faults;
static size_t small, small_peak, committed_peak;
static unsigned threshold;
static void fail(void)
{
    fprintf(stderr, "oracle error %lu\n", GetLastError());
    ExitProcess(3);
}
static void transfer(unsigned pg, int write)
{
    DWORD n = 0;
    if (SetFilePointer(file, pg * PAGE, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER)
        fail();
    BOOL ok = write ? WriteFile(file, base + pg * PAGE, PAGE, &n, NULL)
                    : ReadFile(file, base + pg * PAGE, PAGE, &n, NULL);
    if (!ok || n != PAGE)
        fail();
    if (write)
        writes++;
    else
        reads++;
}
static LONG CALLBACK fault(EXCEPTION_POINTERS *e)
{
    EXCEPTION_RECORD *r = e->ExceptionRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2)
        return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t addr = r->ExceptionInformation[1] - (uintptr_t)base;
    if (addr >= PAGE * PAGES || r->ExceptionInformation[0] > 1)
        return EXCEPTION_CONTINUE_SEARCH;
    unsigned pg = addr / PAGE;
    page_t *p = &map[pg];
    DWORD old;
    if (!p->used)
        return EXCEPTION_CONTINUE_SEARCH;
    faults++;
    if (!p->loaded) {
        unsigned slot;
        if (count == capacity) {
            slot = hand;
            hand = (hand + 1) % capacity;
            unsigned victim = resident[slot];
            page_t *v = &map[victim];
            if (v->dirty) {
                transfer(victim, 1);
                v->stored = 1;
            }
            if (!VirtualFree(base + victim * PAGE, PAGE, MEM_DECOMMIT))
                fail();
            v->dirty = 0;
            v->loaded = 0;
        } else
            slot = count++;
        resident[slot] = pg;
        if (!VirtualAlloc(base + pg * PAGE, PAGE, MEM_COMMIT, PAGE_READWRITE))
            fail();
        if (p->stored)
            transfer(pg, 0);
        p->loaded = 1;
    }
    DWORD prot = r->ExceptionInformation[0] ? PAGE_READWRITE : PAGE_READONLY;
    if (!VirtualProtect(base + pg * PAGE, PAGE, prot, &old))
        fail();
    if (prot == PAGE_READWRITE)
        p->dirty = 1;
    size_t committed = count * PAGE + small;
    if (committed > committed_peak)
        committed_peak = committed;
    return EXCEPTION_CONTINUE_EXECUTION;
}
static void init(void)
{
    capacity = getenv("KVM_RESIDENT_PAGES") ? atoi(getenv("KVM_RESIDENT_PAGES")) : 192;
    threshold = getenv("KVM_PAGED_THRESHOLD") ? atoi(getenv("KVM_PAGED_THRESHOLD")) : 16384;
    if (!capacity || capacity > MAX_RESIDENT || threshold < PAGE)
        fail();
    WCHAR path[MAX_PATH], name[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, path) || !GetTempFileNameW(path, L"kvm", 0, name))
        fail();
    file = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    base = VirtualAlloc(NULL, PAGE * PAGES, MEM_RESERVE, PAGE_NOACCESS);
    if (file == INVALID_HANDLE_VALUE || !base || !AddVectoredExceptionHandler(1, fault))
        fail();
}
void *kvm_probe_malloc(size_t n)
{
    if (!base)
        init();
    if (n < threshold) {
        if (n > SIZE_MAX - 16)
            return NULL;
        size_t *p = _aligned_malloc(n + 16, 16);
        if (!p)
            return NULL;
        *p = n;
        small += n + 16;
        if (small > small_peak)
            small_peak = small;
        size_t committed = count * PAGE + small;
        if (committed > committed_peak)
            committed_peak = committed;
        return (char *)p + 16;
    }
    if (n > PAGE * PAGES)
        return NULL;
    unsigned needed = (n + PAGE - 1) / PAGE, run = 0;
    for (unsigned i = 0; i < PAGES; i++) {
        run = map[i].used ? 0 : run + 1;
        if (run != needed)
            continue;
        unsigned first = i + 1 - run;
        for (unsigned j = first; j <= i; j++)
            map[j].used = 1;
        map[first].run = run;
        map[first].size = n;
        return base + first * PAGE;
    }
    return NULL;
}
void kvm_probe_free(void *ptr)
{
    if (!ptr)
        return;
    uintptr_t off = (uintptr_t)ptr - (uintptr_t)base;
    if (off >= PAGES * PAGE) {
        size_t *p = (size_t *)((char *)ptr - 16);
        small -= *p + 16;
        _aligned_free(p);
        return;
    }
    unsigned start = off / PAGE, len = map[start].run;
    if (off % PAGE || !len)
        fail();
    for (unsigned i = 0; i < count;) {
        unsigned pg = resident[i];
        if (pg < start || pg >= start + len) {
            i++;
            continue;
        }
        if (!VirtualFree(base + pg * PAGE, PAGE, MEM_DECOMMIT))
            fail();
        resident[i] = resident[--count];
    }
    hand = count ? hand % count : 0;
    memset(map + start, 0, len * sizeof(*map));
}
void *kvm_probe_realloc(void *ptr, size_t n)
{
    if (!ptr)
        return kvm_probe_malloc(n);
    uintptr_t off = (uintptr_t)ptr - (uintptr_t)base;
    size_t old = off < PAGE * PAGES ? map[off / PAGE].size : *(size_t *)((char *)ptr - 16);
    void *newp = kvm_probe_malloc(n);
    if (newp) {
        memcpy(newp, ptr, old < n ? old : n);
        kvm_probe_free(ptr);
    }
    return newp;
}
void kvm_probe_report(void)
{
    fprintf(
        stderr,
        "pages=%u threshold=%u small_peak=%zu committed_peak=%zu fault_count=%llu read_bytes=%llu write_bytes=%llu outstanding_small=%zu resident=%u\n",
        capacity, threshold, small_peak, committed_peak, faults, reads * PAGE, writes * PAGE, small, count);
    if (count || small)
        fail();
    if (!CloseHandle(file) || !VirtualFree(base, 0, MEM_RELEASE))
        fail();
}
