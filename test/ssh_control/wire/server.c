// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#endif
#include "ssh_control_session.h"
#include <libssh/server.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const char *scenario;
    uint8_t fingerprint[32];
    int queries, approvals, count, releases;
    bool leased, held, typed;
    unsigned stages;
    const char *end;
} sink_t;

static void diagnostic(void *ctx, kvm_ssh_diag_phase_t stage, const char *end)
{
    sink_t *sink = ctx;
    sink->stages |= 1u << stage;
    if (stage == KVM_SSH_DIAG_END) sink->end = end;
}

static bool paired(void *ctx, const char *user, const uint8_t key[32])
{
    sink_t *s = ctx;
    s->queries++;
    return !strcmp(user, "kvmpad") && !memcmp(key, s->fingerprint, 32);
}

static bool approve(void *ctx, const char *user, const uint8_t key[32])
{
    sink_t *s = ctx;
    s->approvals++;
    s->leased = !strcmp(user, "kvmpad") && !memcmp(key, s->fingerprint, 32) && strcmp(s->scenario, "deny");
    return s->leased;
}

static bool active(void *ctx)
{
    sink_t *s = ctx;
    return s->leased && (strcmp(s->scenario, "revoke") || !s->count);
}

static bool command(void *ctx, const kvm_hid_intent_t *intent)
{
    sink_t *s = ctx;
    if (!active(s)) return false;
    if (s->count == 0) {
        s->typed &= intent->type == KVM_HID_KEY_DOWN && intent->data.key.usage_id == 4;
        s->held = true;
    } else if (s->count == 1) {
        s->typed &= intent->type == KVM_HID_MOUSE_MOVE && intent->data.mouse_move.dx == -2 &&
                    intent->data.mouse_move.dy == 3;
    } else if (s->count == 2) {
        s->typed &= intent->type == KVM_HID_KEY_UP && intent->data.key.usage_id == 4;
        s->held = false;
    }
    s->typed &= intent->origin == KVM_ORIGIN_REMOTE && !intent->epoch && !intent->seq;
    s->count++;
    puts("SINK_ACCEPTED");
    return true;
}

static void release(void *ctx)
{
    sink_t *s = ctx;
    s->held = false;
    s->leased = false;
    s->releases++;
}

static bool heartbeat(void *ctx)
{
    sink_t *s = ctx;
    if (!strcmp(s->scenario, "keepalive")) puts("PEER_ALIVE");
    return s->leased;
}

int main(int argc, char **argv)
{
    if (argc != 4 || ssh_init() != SSH_OK) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    sink_t sink = {.scenario = argv[3], .typed = true};
    ssh_key authorized = NULL;
    unsigned char *hash = NULL;
    size_t len = 0;
    if (ssh_pki_import_pubkey_file(argv[2], &authorized) != SSH_OK ||
        ssh_get_publickey_hash(authorized, SSH_PUBLICKEY_HASH_SHA256, &hash, &len) != SSH_OK || len != 32)
        return 2;
    memcpy(sink.fingerprint, hash, len);
    ssh_clean_pubkey_hash(&hash);
    ssh_key_free(authorized);
    ssh_bind bind = ssh_bind_new();
    ssh_session session = ssh_new();
    bool config = false;
    int port = 0;
    if (!bind || !session || ssh_bind_options_set(bind, SSH_BIND_OPTIONS_PROCESS_CONFIG, &config) != SSH_OK ||
        ssh_bind_options_set(bind, SSH_BIND_OPTIONS_BINDADDR, "127.0.0.1") != SSH_OK ||
        ssh_bind_options_set(bind, SSH_BIND_OPTIONS_BINDPORT, &port) != SSH_OK ||
        ssh_bind_options_set(bind, SSH_BIND_OPTIONS_HOSTKEY, argv[1]) != SSH_OK ||
        ssh_bind_listen(bind) != SSH_OK) return 2;
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    if (getsockname(ssh_bind_get_fd(bind), (struct sockaddr *)&addr, &addrlen) != 0) return 2;
    printf("PORT %u\n", ntohs(addr.sin_port));
    if (ssh_bind_accept(bind, session) != SSH_OK) return 2;
    const kvm_ssh_session_ops_t ops = {.paired = paired, .request_local_approval = approve,
        .active = active, .command = command, .release = release, .peer_activity = heartbeat, .diagnostic = diagnostic};
    bool ok = kvm_ssh_serve_session(session, &ops, &sink);
    printf("RESULT ok=%d queries=%d approvals=%d commands=%d releases=%d held=%d typed=%d\n",
           ok, sink.queries, sink.approvals, sink.count, sink.releases, sink.held, sink.typed);
    printf("DIAG kex=%d query=%d query_ok=%d valid=%d auth=%d channel=%d subsystem=%d shell=%d end=%s\n",
           !!(sink.stages & (1u << KVM_SSH_DIAG_KEX)), !!(sink.stages & (1u << KVM_SSH_DIAG_QUERY)),
           !!(sink.stages & (1u << KVM_SSH_DIAG_QUERY_ACCEPTED)), !!(sink.stages & (1u << KVM_SSH_DIAG_VALID)),
           !!(sink.stages & (1u << KVM_SSH_DIAG_AUTHENTICATED)), !!(sink.stages & (1u << KVM_SSH_DIAG_CHANNEL)),
           !!(sink.stages & (1u << KVM_SSH_DIAG_SUBSYSTEM)), !!(sink.stages & (1u << KVM_SSH_DIAG_SHELL)),
           sink.end ? sink.end : "missing");
    // Match the board: the handler finished the channel, and ssh_free owns the socket.
    ssh_free(session);
    ssh_bind_free(bind);
    ssh_finalize();
    return 0;
}
