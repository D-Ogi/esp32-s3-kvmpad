// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
// Compile pack_scratch.c with PACK_MOUNT="." and run only in a new empty directory.
#include "pack_scratch.h"
#include "pack_card.h"
#include <assert.h>
#include <string.h>

static uint64_t available = 32u * 1024u * 1024u;
static unsigned volume_id = 7;
static uint32_t counter;
static bool mounted = true;

uint32_t esp_random(void) { return ++counter; }

bool pack_card_read_volume(pack_card_volume_t *v, uint64_t *free_bytes)
{
    v->serial = volume_id;
    *free_bytes = available;
    return mounted;
}

bool pack_card_same_volume(const pack_card_volume_t *v) { return mounted && v->serial == volume_id; }

static void expect_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "rb");
    if (!content) {
        assert(!f);
        return;
    }
    char bytes[16] = {0};
    assert(f && fread(bytes, 1, sizeof(bytes), f) == strlen(content) && !strcmp(content, bytes));
    fclose(f);
}

int main(void)
{
    const char *prior = "KV0000000100000002.TMP", *first = "KV0000000300000004.TMP";
    FILE *f = fopen(prior, "wbx");
    assert(f && fputs("prior", f) >= 0 && !fclose(f));
    available--;
    assert(!pack_scratch_open() && counter == 0);
    available++;
    mounted = false;
    assert(!pack_scratch_open() && counter == 0);
    mounted = true;
    f = pack_scratch_open();
    assert(f && pack_scratch_present() && !pack_scratch_open() && counter == 4);
    assert(fputs("scratch", f) >= 0 && pack_scratch_close() && !pack_scratch_close());
    expect_file(first, NULL);
    expect_file(prior, "prior");

    f = pack_scratch_open();
    assert(f && fputs("lost", f) >= 0);
    mounted = false;
    assert(!pack_scratch_present());
    mounted = true;
    assert(!pack_scratch_present() && !pack_scratch_close());
    expect_file("KV0000000500000006.TMP", "lost");

    f = pack_scratch_open();
    assert(f && fputs("swapped", f) >= 0);
    volume_id++;
    assert(!pack_scratch_close());
    expect_file("KV0000000700000008.TMP", "swapped");
    expect_file(prior, "prior");
    puts("PASS scratch capacity, exclusive collision, single owner, Stop, loss and volume change");
}
