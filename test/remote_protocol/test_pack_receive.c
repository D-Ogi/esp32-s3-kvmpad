// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "kvm_test.h"
#include "pack_receive.h"
#include "pack_receive_wire.h"
#include "rp_harness.h"

static uint64_t clock_ms = 1000;
uint64_t pack_receive_test_time(void) { return clock_ms; }
static bool request(void *ctx, uint64_t sid, const char *type, const kvm_json_doc_t *d, int pl,
                    const char **state, uint32_t *offset)
{
    return pack_receive_request(ctx, sid, "Paired desk", type, d, pl, state, offset);
}
static const char offer[] = "'size':3,'sha256':'"
                            "0000000000000000000000000000000000000000000000000000000000000000'";
static uint32_t begin(rph_cli_t *c)
{
    uint32_t e = pack_receive_arm();
    CHECK(e != 0);
    CHECK_EQ(rph_cmd(c, "pack.offer", offer), KVM_RP_OK);
    CHECK(rph_last_has("\"state\":\"pending\""));
    pack_receive_part_t p;
    CHECK(pack_receive_take(e, &p));
    CHECK(p.kind == PACK_RECEIVE_BEGIN && p.size == 3 && !strcmp(p.client, "Paired desk"));
    CHECK(!pack_receive_take(e, &p));
    pack_receive_ack(e, true);
    return e;
}
static void review(rph_cli_t *c, uint32_t e)
{
    CHECK_EQ(rph_cmd(c, "pack.data", "'hex':'00fFA4','offset':0"), KVM_RP_OK);
    CHECK(rph_last_has("\"offset\":0"));
    pack_receive_part_t p;
    CHECK(pack_receive_take(e, &p));
    CHECK(p.kind == PACK_RECEIVE_DATA && p.len == 3 && p.offset == 0 && p.data[0] == 0 &&
          p.data[1] == 255 && p.data[2] == 164);
    pack_receive_ack(e, true);
    CHECK_EQ(rph_cmd(c, "pack.poll", ""), KVM_RP_OK);
    CHECK(rph_last_has("\"offset\":3") && rph_last_has("\"ready\""));
    CHECK_EQ(rph_cmd(c, "pack.end", ""), KVM_RP_OK);
    CHECK(pack_receive_take(e, &p) && p.kind == PACK_RECEIVE_END);
    pack_receive_ack(e, true);
    CHECK(pack_receive_review_active(e));
}

static void lifecycle(void)
{
    rph_reset(NULL);
    g_h.rp.hooks.pack_request = request;
    rph_cli_t c, other;
    rph_cli_open(&c, 501, true);
    rph_cli_open(&other, 502, true);
    CHECK_EQ(rph_cmd(&c, "pack.offer", offer), KVM_RP_OK);
    CHECK(rph_last_has("\"closed\""));
    uint32_t e = begin(&c);
    CHECK(!pack_receive_arm());
    CHECK_EQ(rph_cmd(&other, "pack.poll", ""), KVM_RP_OK);
    CHECK(rph_last_has("\"busy\"") && rph_last_has("\"offset\":0"));
    CHECK_EQ(rph_cmd(&c, "control.request", "'reason':'remote_support'"), KVM_RP_ERR_CONTROL_DENIED);
    review(&c, e);
    pack_receive_finish(e, true);
    CHECK(pack_receive_review_active(e)); // remote completion cannot publish
    CHECK(pack_receive_begin_commit(e));
    pack_receive_cancel(e);
    pack_receive_finish(e, true);
    CHECK_EQ(rph_cmd(&c, "pack.poll", ""), KVM_RP_OK);
    CHECK(rph_last_has("\"installed\""));

    e = begin(&c);
    review(&c, e);
    pack_receive_finish(e, false);
    CHECK_EQ(rph_cmd(&c, "pack.poll", ""), KVM_RP_OK);
    CHECK(rph_last_has("\"denied\""));

    e = begin(&c);
    review(&c, e);
    pack_receive_disconnect(NULL, c.sid);
    CHECK(!pack_receive_begin_commit(e));
    pack_receive_part_t part;
    CHECK(pack_receive_take(e, &part) && part.kind == PACK_RECEIVE_ABORT);
    CHECK_EQ(rph_cmd(&c, "pack.poll", ""), KVM_RP_OK);
    CHECK(rph_last_has("\"cancelled\""));

    uint32_t old = e;
    e = begin(&c);
    pack_receive_cancel(old);
    CHECK_EQ(rph_cmd(&c, "pack.data", "'offset':1,'hex':'ff'"), KVM_RP_ERR_BAD_FRAME);
    CHECK(pack_receive_take(e, &part) && part.kind == PACK_RECEIVE_ABORT);
    e = begin(&c);
    review(&c, e);
    clock_ms += 90001;
    CHECK(!pack_receive_begin_commit(e));
    CHECK_EQ(rph_cmd(&c, "pack.poll", ""), KVM_RP_OK);
    CHECK(rph_last_has("\"expired\""));
    e = begin(&c);
    CHECK_EQ(rph_cmd(&c, "pack.cancel", ""), KVM_RP_OK);
    CHECK(rph_last_has("\"cancelled\""));
    CHECK(pack_receive_take(e, &part) && part.kind == PACK_RECEIVE_ABORT);
}

static void bounds_and_roles(void)
{
    rph_reset(NULL);
    g_h.rp.hooks.pack_request = request;
    rph_cli_t c;
    rph_cli_open(&c, 783, false);
    CHECK_EQ(rph_cmd(&c, "pack.offer", offer), KVM_RP_ERR_CONTROL_DENIED);
    rph_cli_open(&c, 784, true);
    CHECK_EQ(rph_cmd(&c, "pack.offer", "'size':262145,'sha256':'ff'"), KVM_RP_ERR_BAD_FRAME);
    uint32_t e = begin(&c);
    CHECK_EQ(rph_cmd(&c, "pack.data", "'offset':0,'hex':'aa','path':'test'"), KVM_RP_ERR_BAD_FRAME);
    CHECK_EQ(rph_cmd(&c, "pack.data", "'offset':0,'hex':'aa0'"), KVM_RP_ERR_BAD_FRAME);
    CHECK_EQ(rph_cmd(&c, "pack.end", ""), KVM_RP_ERR_BAD_FRAME);
    pack_receive_part_t part;
    CHECK(pack_receive_take(e, &part) && part.kind == PACK_RECEIVE_ABORT);
    e = begin(&c);
    clock_ms += 30001;
    CHECK(pack_receive_take(e, &part) && part.kind == PACK_RECEIVE_ABORT);
    CHECK_EQ(rph_cmd(&c, "pack.poll", ""), KVM_RP_OK);
    CHECK(rph_last_has("\"expired\""));
}

// Synthetic stdio transport for the desktop SDK interop test. Only this test
// executable simulates physical approval. Every received byte is compared with
// the test archive.
static int peer(const char *path, const char *sha)
{
    FILE *f = fopen(path, "rb");
    if (!f || strlen(sha) != 64)
        return 2;
    rph_reset(NULL);
    g_h.rp.hooks.pack_request = request;
    kvm_rp_session_info_t info = {
        .session_id = 1, .authenticated = true, .operator_role = true, .label_entropy = 123};
    if (kvm_rp_session_open(&g_h.rp, &info, g_h.now) < 0)
        return 2;
    uint32_t e = pack_receive_arm(), written = 0;
    unsigned reviews = 0;
    bool installed = false;
    char line[KVM_JSON_MAX_FRAME + 3];
    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\r\n")] = 0;
        clock_ms += 10;
        g_h.now = clock_ms * 1000;
        int previous = g_h.n_frame;
        if (rph_feed(1, line) != KVM_RP_OK || g_h.n_frame != previous + 1)
            return 3;
        puts(g_h.frame[(g_h.n_frame - 1) % RPH_MAX_FRAMES]);
        fflush(stdout);
        pack_receive_part_t part;
        if (pack_receive_take(e, &part)) {
            if (part.kind == PACK_RECEIVE_BEGIN) {
                char actual[65];
                for (unsigned i = 0; i < 32; i++)
                    snprintf(actual + 2 * i, 3, "%02X", part.sha256[i]);
                if (strcmp(actual, sha))
                    return 4;
            } else if (part.kind == PACK_RECEIVE_DATA) {
                uint8_t expected[256];
                if (part.offset != written || fread(expected, 1, part.len, f) != part.len ||
                    memcmp(expected, part.data, part.len))
                    return 5;
                written += part.len;
            } else if (part.kind != PACK_RECEIVE_END || fgetc(f) != EOF)
                return 6;
            pack_receive_ack(e, true);
        } else if (pack_receive_review_active(e) && ++reviews == 2) {
            if (!pack_receive_begin_commit(e))
                return 7;
            pack_receive_finish(e, true);
            installed = true;
        }
    }
    fclose(f);
    return installed && written ? 0 : 8;
}

int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "--peer"))
        return peer(argv[2], argv[3]);
    RUN(lifecycle);
    RUN(bounds_and_roles);
    TEST_MAIN_END("pack_receive");
}
