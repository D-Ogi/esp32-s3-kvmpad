// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static void fill(uint8_t *block, size_t n, unsigned index)
{
    for (size_t i = 0; i < n; i++)
        block[i] = (uint8_t)(i * 37u + index * 67u);
}

bool card_ram_roundtrip(void)
{
    char path[24];
    static FIL file;
    unsigned seed = (unsigned)xTaskGetTickCount();
    FRESULT result = FR_EXIST;
    for (unsigned i = 0; result == FR_EXIST && i < 16; ++i) {
        snprintf(path, sizeof(path), "0:/KV%06X.TST", (seed + i) & 0xffffff);
        result = f_open(&file, path, FA_CREATE_NEW | FA_WRITE);
    }
    if (result != FR_OK)
        return false;
    // Only this invocation's newly created file can reach cleanup.
    static uint8_t expected[513], actual[513];
    UINT length;
    bool ok = true;
    for (unsigned b = 0; ok && b < 16; ++b) {
        fill(expected, sizeof(expected), b);
        ok = f_write(&file, expected, sizeof(expected), &length) == FR_OK && length == sizeof(expected);
    }
    if (ok)
        ok = f_sync(&file) == FR_OK;
    if (f_close(&file) != FR_OK)
        ok = false;
    if (ok) {
        ok = f_open(&file, path, FA_READ) == FR_OK;
        if (ok) {
            ok = f_size(&file) == 16 * sizeof(expected);
            for (unsigned b = 0; ok && b < 16; ++b) {
                fill(expected, sizeof(expected), b);
                ok = f_read(&file, actual, sizeof(actual), &length) == FR_OK && length == sizeof(actual) &&
                     memcmp(expected, actual, sizeof(actual)) == 0;
            }
            if (f_close(&file) != FR_OK)
                ok = false;
        }
    }
    FRESULT removed = f_unlink(path);
    static FILINFO info;
    bool absent = removed == FR_OK && f_stat(path, &info) == FR_NO_FILE;
    printf("CARD_FILE bytes=8208 readback=%d removed=%d\n", (int)ok, (int)absent);
    if (!absent)
        printf("CARD_FILE leftover=%s\n", path);
    return ok && absent;
}
