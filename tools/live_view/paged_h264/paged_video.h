// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#ifndef KVM_PAGED_VIDEO_H
#define KVM_PAGED_VIDEO_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
typedef struct kvm_paged_vm kvm_paged_vm_t;
typedef struct {
    unsigned width, height;
    uint32_t plane[3], stride[3];
    bool full_range, bt709;
} kvm_paged_picture_t;
typedef struct {
    uint64_t steps, page_reads, page_writes;
    uint32_t frames, last_pc, scratch_bytes, rom_loads;
    int guest_status;
} kvm_paged_stats_t;
// These callbacks run synchronously on the worker. Read plane rows before returning.
typedef struct {
    void *ctx;
    bool (*frame)(void *, kvm_paged_vm_t *, const kvm_paged_picture_t *);
    bool (*stop)(void *);
} kvm_paged_sink_t;
bool kvm_paged_read(kvm_paged_vm_t *, uint32_t guest_address, void *dst, size_t bytes);
// Caller supplies an immutable KVR1 image or little-endian RV32 ELF, an original Annex B stream,
// and a distinct seekable, exclusive scratch file. The VM owns no filesystem paths.
int kvm_paged_run(const void *elf, size_t elf_size, FILE *source, FILE *scratch, unsigned cache_pages,
                  bool large_bank, const kvm_paged_sink_t *, kvm_paged_stats_t *);
#endif
