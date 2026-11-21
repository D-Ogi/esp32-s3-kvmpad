// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "pack_scratch.h"
#include "pack_card.h"
#include "esp_random.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <unistd.h>

static FILE *active;
static bool lost;
static pack_card_volume_t volume;
static char path[sizeof(PACK_MOUNT "/KVFFFFFFFFFFFFFFFF.TMP")];

bool pack_scratch_present(void)
{
    if (!lost && !pack_card_same_volume(&volume))
        lost = true;
    return !lost;
}

FILE *pack_scratch_open(void)
{
    return pack_scratch_open_for(32u * 1024u * 1024u);
}

FILE *pack_scratch_open_for(uint32_t bytes)
{
    uint64_t free_bytes = 0;
    if (!bytes || active || !pack_card_read_volume(&volume, &free_bytes) || free_bytes < bytes)
        return NULL;
    lost = false;
    int flags = O_CREAT | O_EXCL | O_RDWR;
#ifdef O_BINARY
    flags |= O_BINARY;
#endif
    for (unsigned attempt = 0; attempt < 8 && pack_scratch_present(); ++attempt) {
        uint32_t first = esp_random(), second = esp_random();
        snprintf(path, sizeof(path), PACK_MOUNT "/KV%08" PRIX32 "%08" PRIX32 ".TMP", first, second);
        int fd = open(path, flags, 0600);
        if (fd < 0) {
            if (errno == EEXIST)
                continue;
            return NULL;
        }
        active = fdopen(fd, "r+b");
        if (!active) {
            close(fd);
            if (pack_scratch_present())
                unlink(path);
            return NULL;
        }
        setvbuf(active, NULL, _IONBF, 0);
        return active;
    }
    return NULL;
}

bool pack_scratch_close(void)
{
    if (!active)
        return false;
    // Latch an observed card loss, including removal during Stop or flush.
    bool same = pack_scratch_present();
    bool closed = fclose(active) == 0;
    active = NULL;
    return same && pack_scratch_present() && unlink(path) == 0 && closed;
}
