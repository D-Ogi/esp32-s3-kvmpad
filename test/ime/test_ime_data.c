// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// ime_data directory parser (TECH-081): a valid image as tools/ime/build_ime_data.py writes it, and every way a
// directory can be wrong (erased flash, wrong version, counts, names, alignment, bounds, overlap, overflow) is
// refused with its own status and leaves no entry behind.
#include <stdint.h>
#include <string.h>

#include "ime_data.h"
#include "kvm_test.h"

#define PART (6u * 1024u * 1024u)

static uint8_t s_dir[IME_DATA_DIR_LEN];

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void set_entry(uint32_t i, const char *name, uint32_t off, uint32_t size)
{
    uint8_t *e = s_dir + IME_DATA_HEADER_LEN + i * IME_DATA_ENTRY_LEN;
    memset(e, 0, IME_DATA_ENTRY_LEN);
    memcpy(e, name, strlen(name));
    wr32(e + 24, off);
    wr32(e + 28, size);
    memset(e + 32, (int)(0xA0u + i), 32);
}

static void image(uint32_t count)
{
    memset(s_dir, 0xFF, sizeof(s_dir));
    memcpy(s_dir, IME_DATA_MAGIC, 8);
    wr32(s_dir + 8, IME_DATA_VERSION);
    wr32(s_dir + 12, count);
    wr32(s_dir + 16, 0);
    wr32(s_dir + 20, 0);
    set_entry(0, "dict_pinyin.dat", 4096u, 1068442u);
    if (count > 1) {
        set_entry(1, "dict_ja.dat", 4096u + 1069056u, 5000u);
    }
}

static ime_data_status_t parse(uint32_t *n, ime_data_entry_t *e)
{
    return ime_data_parse(s_dir, sizeof(s_dir), PART, e, IME_DATA_MAX_ENTRIES, n);
}

static void test_valid(void)
{
    ime_data_entry_t e[IME_DATA_MAX_ENTRIES];
    uint32_t n = 99;
    image(2);
    CHECK_EQ(parse(&n, e), IME_DATA_OK);
    CHECK_EQ(n, 2u);
    CHECK(strcmp(e[0].name, "dict_pinyin.dat") == 0);
    CHECK_EQ(e[0].offset, 4096u);
    CHECK_EQ(e[0].size, 1068442u);
    CHECK_EQ(e[0].sha256[0], 0xA0u);
    CHECK_EQ(e[1].sha256[31], 0xA1u);
    // a file that ends exactly at the partition end
    image(1);
    set_entry(0, "a", PART - 8192u, 8192u);
    CHECK_EQ(parse(&n, e), IME_DATA_OK);
}

static void expect(ime_data_status_t want, const char *why)
{
    ime_data_entry_t e[IME_DATA_MAX_ENTRIES];
    uint32_t n = 99;
    const ime_data_status_t got = parse(&n, e);
    if (got != want) {
        printf("    %s: %s, expected %s\n", why, ime_data_status_name(got), ime_data_status_name(want));
    }
    CHECK_EQ(got, want);
    CHECK_EQ(n, 0u); // nothing usable leaks out of a refused directory
}

static void test_refusals(void)
{
    memset(s_dir, 0xFF, sizeof(s_dir));
    expect(IME_DATA_ERR_MAGIC, "erased flash");

    image(1);
    wr32(s_dir + 8, 2);
    expect(IME_DATA_ERR_VERSION, "version 2");
    image(1);
    wr32(s_dir + 20, 1);
    expect(IME_DATA_ERR_VERSION, "reserved word set");

    image(1);
    wr32(s_dir + 12, 0);
    expect(IME_DATA_ERR_COUNT, "no file");
    image(1);
    wr32(s_dir + 12, IME_DATA_MAX_ENTRIES + 1);
    expect(IME_DATA_ERR_COUNT, "too many files");

    image(1);
    set_entry(0, "", 4096u, 10u);
    expect(IME_DATA_ERR_NAME, "empty name");
    image(1);
    set_entry(0, "Dict.dat", 4096u, 10u);
    expect(IME_DATA_ERR_NAME, "upper case");
    image(1);
    set_entry(0, "../x", 4096u, 10u);
    expect(IME_DATA_ERR_NAME, "path separator");
    image(1);
    memset(s_dir + IME_DATA_HEADER_LEN, 'a', IME_DATA_NAME_CAP);
    expect(IME_DATA_ERR_NAME, "not terminated");
    image(1);
    s_dir[IME_DATA_HEADER_LEN + 20] = 'x'; // garbage after the terminator
    expect(IME_DATA_ERR_NAME, "padding not zero");
    image(2);
    set_entry(1, "dict_pinyin.dat", 8u * 4096u * 1024u / 4u, 10u);
    expect(IME_DATA_ERR_NAME, "duplicate name");

    image(1);
    set_entry(0, "a", 0u, 10u);
    expect(IME_DATA_ERR_BOUNDS, "inside the directory page");
    image(1);
    set_entry(0, "a", 4097u, 10u);
    expect(IME_DATA_ERR_BOUNDS, "unaligned");
    image(1);
    set_entry(0, "a", 4096u, 0u);
    expect(IME_DATA_ERR_BOUNDS, "empty file");
    image(1);
    set_entry(0, "a", PART - 4096u, 4097u);
    expect(IME_DATA_ERR_BOUNDS, "past the end");
    image(1);
    set_entry(0, "a", 4096u, UINT32_MAX);
    expect(IME_DATA_ERR_BOUNDS, "size overflow");
    image(1);
    set_entry(0, "a", 0xFFFFF000u, 4096u);
    expect(IME_DATA_ERR_BOUNDS, "offset past the partition");
    image(2);
    set_entry(1, "b", 8192u, 10u); // inside dict_pinyin.dat
    expect(IME_DATA_ERR_BOUNDS, "overlap");

    ime_data_entry_t e[1];
    uint32_t n = 0;
    image(1);
    CHECK_EQ(ime_data_parse(s_dir, IME_DATA_DIR_LEN - 1u, PART, e, 1, &n), IME_DATA_ERR_ARG);
    CHECK_EQ(ime_data_parse(NULL, IME_DATA_DIR_LEN, PART, e, 1, &n), IME_DATA_ERR_ARG);
    image(2);
    CHECK_EQ(ime_data_parse(s_dir, sizeof(s_dir), PART, e, 1, &n), IME_DATA_ERR_COUNT); // cap below count
}

int main(void)
{
    RUN(test_valid);
    RUN(test_refusals);
    TEST_MAIN_END("test_ime_data");
}
