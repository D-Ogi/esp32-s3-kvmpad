// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "pack_store.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>

static enum { NO_FAULT, READ_ERROR, STAT_ERROR, CLOSE_ERROR } fault;
static const char *fault_path;
static DIR *target_dir;
static bool injected;

static DIR *open_dir(const char *path)
{
    DIR *dir = opendir(path);
    if (fault_path && !strcmp(path, fault_path)) target_dir = dir;
    return dir;
}

static struct dirent *read_dir(DIR *dir)
{
    struct dirent *entry = readdir(dir);
    if (!entry && dir == target_dir && fault == READ_ERROR) {
        injected = true;
        errno = EIO;
    }
    return entry;
}

static int close_dir(DIR *dir)
{
    bool selected = dir == target_dir;
    if (selected) target_dir = NULL;
    int result = closedir(dir);
    if (selected && fault == CLOSE_ERROR) {
        injected = true;
        errno = EIO;
        return -1;
    }
    return result;
}

static int stat_file(const char *path, struct stat *st)
{
    if (fault == STAT_ERROR && !strcmp(path, fault_path)) {
        injected = true;
        errno = EIO;
        return -1;
    }
    return stat(path, st);
}

// Replace only filesystem calls made by the production directory scanner.
#define opendir open_dir
#define readdir read_dir
#define closedir close_dir
#define stat(path, st) stat_file(path, st)
#include "pack_store.c"

static void list(pack_list_kind_t kind, unsigned expected_count)
{
    pack_item_t items[PACK_PAGE];
    unsigned count = 99;
    bool more = true;
    injected = false;
    errno = EIO;
    esp_err_t result = pack_card_list_kind(items, kind, 0, &count, &more);
    assert(injected == (fault != NO_FAULT));
    assert(result == (injected ? ESP_FAIL : ESP_OK));
    assert(count == expected_count && !more);
    if (count) assert(items[0].movie && items[0].prepared_modes == 0);
}

int main(void)
{
    list(PACK_LIST_VIDEO, 1);
    list(PACK_LIST_INSTALLED, 0);
    fault_path = PACK_MOUNT;
    fault = READ_ERROR;
    list(PACK_LIST_VIDEO, 0);
    fault = CLOSE_ERROR;
    list(PACK_LIST_VIDEO, 0);
    fault_path = ROOT "/01234567";
    list(PACK_LIST_INSTALLED, 0);
    fault = READ_ERROR;
    list(PACK_LIST_INSTALLED, 0);
    fault = STAT_ERROR;
    list(PACK_LIST_INSTALLED, 0);
    fault_path = PACK_MOUNT "/sample.h264";
    list(PACK_LIST_VIDEO, 0);
    fault_path = PACK_MOUNT "/sample.h264.kvc";
    list(PACK_LIST_VIDEO, 0);
    puts("Card browser passed 7 directory, file and companion I/O faults and 2 normal listings.");
    return 0;
}
