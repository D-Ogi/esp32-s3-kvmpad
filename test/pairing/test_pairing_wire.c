// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Host tests of the `pair.*` encoding helpers (TASK-100, ADR-029 §4): base64, hex, the
// length-prefixed round blobs and the mDNS TXT record. AT-EXT-17 lives on the malformed-input side
// of these: every reject here is a frame a fuzzer can feed the transport.

#include <string.h>

#include "kvm_pairing_wire.h"
#include "kvm_test.h"

static void test_b64_roundtrip(void)
{
    for (size_t len = 1; len <= 200; len++) {
        uint8_t in[200];
        for (size_t i = 0; i < len; i++) {
            in[i] = (uint8_t)(i * 7u + len);
        }
        char enc[400];
        CHECK(kvm_pair_b64_encode(in, len, enc, sizeof(enc)));
        CHECK_EQ(strlen(enc), KVM_PAIR_B64_LEN(len));

        uint8_t back[200];
        size_t back_len = 0;
        CHECK(kvm_pair_b64_decode(enc, back, sizeof(back), &back_len));
        CHECK_EQ(back_len, len);
        CHECK(memcmp(in, back, len) == 0);
    }
}

static void test_b64_known_vectors(void)
{
    // RFC 4648 §10 test vectors: the format is standard base64, nothing custom.
    const char *plain[] = { "f", "fo", "foo", "foob", "fooba", "foobar" };
    const char *coded[] = { "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy" };
    for (size_t i = 0; i < 6; i++) {
        char enc[16];
        CHECK(kvm_pair_b64_encode((const uint8_t *)plain[i], strlen(plain[i]), enc, sizeof(enc)));
        CHECK(strcmp(enc, coded[i]) == 0);

        uint8_t back[16];
        size_t back_len = 0;
        CHECK(kvm_pair_b64_decode(coded[i], back, sizeof(back), &back_len));
        CHECK_EQ(back_len, strlen(plain[i]));
        CHECK(memcmp(back, plain[i], back_len) == 0);
    }
}

static void test_b64_rejects(void)
{
    uint8_t out[64];
    size_t out_len = 0;
    // Bad length, bad alphabet, padding in the wrong place, whitespace, empty.
    CHECK(!kvm_pair_b64_decode("Zm9", out, sizeof(out), &out_len));
    CHECK(!kvm_pair_b64_decode("Zm9*", out, sizeof(out), &out_len));
    CHECK(!kvm_pair_b64_decode("Z=9v", out, sizeof(out), &out_len));
    CHECK(!kvm_pair_b64_decode("=m9v", out, sizeof(out), &out_len));
    CHECK(!kvm_pair_b64_decode("Zm9v Zm9v", out, sizeof(out), &out_len));
    CHECK(!kvm_pair_b64_decode("Zm9v\n", out, sizeof(out), &out_len));
    CHECK(!kvm_pair_b64_decode("", out, sizeof(out), &out_len));
    // Padding only in the last group.
    CHECK(!kvm_pair_b64_decode("Zg==Zg==", out, sizeof(out), &out_len));
    // Decoded output longer than the buffer.
    CHECK(!kvm_pair_b64_decode("Zm9vYmFy", out, 4, &out_len));
    // Encoding into a short buffer.
    char small[4];
    CHECK(!kvm_pair_b64_encode((const uint8_t *)"foo", 3, small, sizeof(small)));
}

static void test_hex(void)
{
    const uint8_t in[4] = { 0x00, 0x0f, 0xa5, 0xff };
    char enc[16];
    CHECK(kvm_pair_hex_encode(in, sizeof(in), enc, sizeof(enc)));
    CHECK(strcmp(enc, "000fa5ff") == 0);

    uint8_t back[4];
    size_t back_len = 0;
    CHECK(kvm_pair_hex_decode("000FA5ff", back, sizeof(back), &back_len));
    CHECK_EQ(back_len, 4);
    CHECK(memcmp(back, in, 4) == 0);

    CHECK(!kvm_pair_hex_decode("abc", back, sizeof(back), &back_len));
    CHECK(!kvm_pair_hex_decode("abcg", back, sizeof(back), &back_len));
    CHECK(!kvm_pair_hex_decode("", back, sizeof(back), &back_len));
    CHECK(!kvm_pair_hex_decode("0011223344", back, 4, &back_len));
}

static void test_blob_roundtrip(void)
{
    uint8_t buf[KVM_PAIR_ROUND1_MAX];
    kvm_pair_blob_t w;
    kvm_pair_blob_begin(&w, buf, sizeof(buf));

    uint8_t point[65];
    uint8_t proof[32];
    memset(point, 0x04, sizeof(point));
    memset(proof, 0x11, sizeof(proof));
    for (int s = 0; s < 2; s++) {
        CHECK(kvm_pair_blob_put(&w, point, sizeof(point)));
        CHECK(kvm_pair_blob_put(&w, point, sizeof(point)));
        CHECK(kvm_pair_blob_put(&w, proof, sizeof(proof)));
    }
    CHECK(!w.overflow);
    CHECK_EQ(w.len, 6u + 4u * 65u + 2u * 32u);
    CHECK(w.len <= KVM_PAIR_ROUND1_MAX);

    const uint8_t *ptr[KVM_PAIR_ROUND1_CHUNKS];
    size_t len[KVM_PAIR_ROUND1_CHUNKS];
    CHECK(kvm_pair_blob_split(buf, w.len, KVM_PAIR_ROUND1_CHUNKS, ptr, len));
    CHECK_EQ(len[0], 65);
    CHECK_EQ(len[2], 32);
    CHECK(memcmp(ptr[0], point, 65) == 0);
    CHECK(memcmp(ptr[2], proof, 32) == 0);
}

static void test_blob_rejects(void)
{
    const uint8_t *ptr[KVM_PAIR_ROUND1_CHUNKS];
    size_t len[KVM_PAIR_ROUND1_CHUNKS];

    // Truncated: the length byte promises more than the blob holds.
    const uint8_t truncated[4] = { 3, 1, 2 };
    CHECK(!kvm_pair_blob_split(truncated, 3, 1, ptr, len));
    // Zero-length chunk.
    const uint8_t zero_chunk[2] = { 0, 0 };
    CHECK(!kvm_pair_blob_split(zero_chunk, 2, 1, ptr, len));
    // Trailing bytes after the last chunk: a malformed frame, not something to ignore.
    const uint8_t trailing[4] = { 1, 0xaa, 0xbb, 0xcc };
    CHECK(!kvm_pair_blob_split(trailing, 4, 1, ptr, len));
    // Chunk longer than a P-256 point.
    uint8_t too_long[80];
    too_long[0] = 70;
    CHECK(!kvm_pair_blob_split(too_long, 71, 1, ptr, len));
    // Empty blob, and fewer chunks than asked for.
    CHECK(!kvm_pair_blob_split(trailing, 0, 1, ptr, len));
    const uint8_t one_chunk[2] = { 1, 0xaa };
    CHECK(!kvm_pair_blob_split(one_chunk, 2, 2, ptr, len));

    // Writer refuses an over-long chunk and a full buffer.
    uint8_t small[8];
    kvm_pair_blob_t w;
    kvm_pair_blob_begin(&w, small, sizeof(small));
    uint8_t big[66];
    memset(big, 0, sizeof(big));
    CHECK(!kvm_pair_blob_put(&w, big, sizeof(big)));
    CHECK(w.overflow);
    kvm_pair_blob_begin(&w, small, sizeof(small));
    CHECK(!kvm_pair_blob_put(&w, big, 0));
    kvm_pair_blob_begin(&w, small, sizeof(small));
    CHECK(kvm_pair_blob_put(&w, big, 7));
    CHECK(!kvm_pair_blob_put(&w, big, 7));
}

static void test_sizes_fit_frame(void)
{
    // 03 §6.5 caps a frame at 1024 bytes. The largest `pair.*` payload is round 1, base64-encoded.
    CHECK(KVM_PAIR_ROUND1_B64_CAP <= 450);
    CHECK(KVM_PAIR_ROUND2_B64_CAP <= 230);
    CHECK(KVM_PAIR_TOKEN_B64_CAP <= 70);
    // The longest message is `pair.round1`: the envelope keys of 03 §6.2 (`v`, `type`, `seq`,
    // `session_id`), the base64 blob and the device name. 256 bytes is a generous allowance for
    // everything except the blob, and the total still has to fit the 1024-byte cap with room left.
    CHECK(KVM_PAIR_ROUND1_B64_CAP + 256u <= 1024u);
    CHECK(KVM_PAIR_ROUND2_B64_CAP + 256u <= 1024u);
    CHECK(KVM_PAIR_TOKEN_B64_CAP + KVM_PAIR_NAME_CAP + 256u <= 1024u);
}

static void test_mdns_txt(void)
{
    kvm_pair_txt_item_t items[KVM_PAIR_MDNS_TXT_ITEMS];
    char scratch[32];

    CHECK(kvm_pair_mdns_txt("KVMpad-7F3A", true, items, scratch, sizeof(scratch)));
    CHECK(strcmp(items[0].name, "name") == 0);
    CHECK(strcmp(items[0].value, "KVMpad-7F3A") == 0);
    CHECK(strcmp(items[1].name, "v") == 0);
    CHECK(strcmp(items[1].value, "1") == 0);
    CHECK(strcmp(items[2].name, "pair") == 0);
    CHECK(strcmp(items[2].value, "1") == 0);

    CHECK(kvm_pair_mdns_txt("KVMpad-7F3A", false, items, scratch, sizeof(scratch)));
    CHECK(strcmp(items[2].value, "0") == 0);

    CHECK(!kvm_pair_mdns_txt("", true, items, scratch, sizeof(scratch)));
    CHECK(!kvm_pair_mdns_txt("KVMpad-7F3A", true, items, scratch, 2));
    CHECK(strcmp(KVM_PAIR_MDNS_FQDN, "_kvmpad._tcp.local") == 0);
}

int main(void)
{
    printf("test_pairing_wire\n");
    RUN(test_b64_roundtrip);
    RUN(test_b64_known_vectors);
    RUN(test_b64_rejects);
    RUN(test_hex);
    RUN(test_blob_roundtrip);
    RUN(test_blob_rejects);
    RUN(test_sizes_fit_frame);
    RUN(test_mdns_txt);
    TEST_MAIN_END("test_pairing_wire");
}
