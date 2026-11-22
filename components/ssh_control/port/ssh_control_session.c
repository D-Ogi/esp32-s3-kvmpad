// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "ssh_control_session.h"
#include <libssh/callbacks.h>
#include <libssh/server.h>
#include <string.h>
#include <time.h>

enum { SETUP_MS = 15000, IDLE_MS = 30000, POLL_MS = 50, USER_MAX = 64 };

typedef struct {
    const kvm_ssh_session_ops_t *ops;
    void *ctx;
    ssh_channel channel;
    struct ssh_channel_callbacks_struct channel_cb;
    char user[USER_MAX + 1];
    uint8_t hash[32];
    char line[KVM_SSH_COMMAND_MAX];
    size_t used;
    uint64_t deadline;
    bool authenticated, requested, granted, failed, eof, remote_closed;
} control_t;

static void diagnostic(control_t *s, kvm_ssh_diag_phase_t stage, const char *end_reason)
{
    if (s->ops->diagnostic) s->ops->diagnostic(s->ctx, stage, end_reason);
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return UINT64_MAX;
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static int auth(ssh_session session, const char *user, ssh_key key, char signature, void *data)
{
    (void)session;
    control_t *s = data;
    if (s->authenticated || !user || !*user || strnlen(user, USER_MAX + 1) > USER_MAX ||
        (signature != SSH_PUBLICKEY_STATE_NONE && signature != SSH_PUBLICKEY_STATE_VALID))
        return SSH_AUTH_DENIED;
    unsigned char *hash = NULL;
    size_t len = 0;
    diagnostic(s, signature == SSH_PUBLICKEY_STATE_NONE ? KVM_SSH_DIAG_QUERY : KVM_SSH_DIAG_VALID, NULL);
    bool known = ssh_get_publickey_hash(key, SSH_PUBLICKEY_HASH_SHA256, &hash, &len) == SSH_OK &&
                 len == sizeof(s->hash) && s->ops->paired(s->ctx, user, hash);
    if (known && signature == SSH_PUBLICKEY_STATE_NONE)
        diagnostic(s, KVM_SSH_DIAG_QUERY_ACCEPTED, NULL);
    if (known && signature == SSH_PUBLICKEY_STATE_VALID) {
        memcpy(s->hash, hash, sizeof(s->hash));
        strcpy(s->user, user);
        s->authenticated = true;
        diagnostic(s, KVM_SSH_DIAG_AUTHENTICATED, NULL);
    }
    ssh_clean_pubkey_hash(&hash);
    // For NONE libssh sends PK_OK only. Channel access requires the VALID path.
    return known ? SSH_AUTH_SUCCESS : SSH_AUTH_DENIED;
}

static int start(ssh_session session, ssh_channel channel, void *data)
{
    (void)session;
    control_t *s = data;
    if (!s->authenticated || s->requested || channel != s->channel || s->failed)
        return -1;
    s->requested = true;
    s->granted = s->ops->request_local_approval(s->ctx, s->user, s->hash) && s->ops->active(s->ctx);
    if (!s->granted) {
        s->failed = true;
        return -1;
    }
    s->deadline = now_ms() + IDLE_MS;
    return 0;
}

static int subsystem(ssh_session session, ssh_channel channel, const char *name, void *data)
{
    if (!name || strcmp(name, "kvmpad") != 0) return -1;
    diagnostic(data, KVM_SSH_DIAG_SUBSYSTEM, NULL);
    return start(session, channel, data);
}

static int shell(ssh_session session, ssh_channel channel, void *data)
{
    diagnostic(data, KVM_SSH_DIAG_SHELL, NULL);
    return start(session, channel, data);
}

static int input(ssh_session session, ssh_channel channel, void *buffer, uint32_t size,
                 int stderr_stream, void *data)
{
    (void)session;
    control_t *s = data;
    const char *bytes = buffer;
    if (!s->granted || channel != s->channel || stderr_stream || s->eof) s->failed = true;
    for (uint32_t i = 0; i < size && !s->failed; i++) {
        if (s->used == sizeof(s->line)) {
            s->failed = true;
            break;
        }
        s->line[s->used++] = bytes[i];
        if (bytes[i] != '\n') continue;
        kvm_hid_intent_t intent;
        if (!kvm_ssh_parse_command(s->line, s->used, &intent) || !s->ops->active(s->ctx) ||
            !s->ops->command(s->ctx, &intent) || (s->ops->peer_activity && !s->ops->peer_activity(s->ctx))) {
            s->failed = true;
            break;
        }
        s->used = 0;
        s->deadline = now_ms() + IDLE_MS;
    }
    return (int)size;
}

static void channel_eof(ssh_session session, ssh_channel channel, void *data)
{
    (void)session;
    (void)channel;
    ((control_t *)data)->eof = true;
}

static void channel_closed(ssh_session session, ssh_channel channel, void *data)
{
    (void)session;
    (void)channel;
    ((control_t *)data)->remote_closed = true;
}

static void global_request(ssh_session session, ssh_message message, void *data)
{
    (void)session;
    control_t *s = data;
    if (ssh_message_subtype(message) == SSH_GLOBAL_REQUEST_KEEPALIVE && s->authenticated) {
        if (s->granted && s->ops->peer_activity && !s->ops->peer_activity(s->ctx)) s->failed = true;
        if (!s->failed) {
            if (s->granted) s->deadline = now_ms() + IDLE_MS;
            ssh_message_global_request_reply_success(message, 0);
            return;
        }
    }
    ssh_message_reply_default(message);
}

static ssh_channel open_session(ssh_session session, void *data)
{
    control_t *s = data;
    if (!s->authenticated || s->failed || s->channel) return NULL;
    s->channel = ssh_channel_new(session);
    if (s->channel && ssh_set_channel_callbacks(s->channel, &s->channel_cb) != SSH_OK) {
        ssh_channel_free(s->channel);
        s->channel = NULL;
        s->failed = true;
    }
    if (s->channel) diagnostic(s, KVM_SSH_DIAG_CHANNEL, NULL);
    return s->channel;
}

bool kvm_ssh_serve_session(ssh_session session, const kvm_ssh_session_ops_t *ops, void *ctx)
{
    if (!session || !ops || !ops->paired || !ops->request_local_approval || !ops->active ||
        !ops->command || !ops->release) return false;
    control_t s = {.ops = ops, .ctx = ctx, .deadline = now_ms() + SETUP_MS};
    diagnostic(&s, KVM_SSH_DIAG_SETUP, NULL);
    struct ssh_server_callbacks_struct cb = {.userdata = &s, .auth_pubkey_function = auth,
        .channel_open_request_session_function = open_session};
    ssh_callbacks_init(&cb);
    s.channel_cb = (struct ssh_channel_callbacks_struct){.userdata = &s, .channel_data_function = input,
        .channel_eof_function = channel_eof, .channel_close_function = channel_closed, .channel_shell_request_function = shell,
        .channel_subsystem_request_function = subsystem};
    ssh_callbacks_init(&s.channel_cb);
    ssh_event event = ssh_event_new();
    bool ok = false;
    bool completed_stream;
    const char *end = "setup";
    if (!event) {
        diagnostic(&s, KVM_SSH_DIAG_END, end);
        return false;
    }
    struct ssh_callbacks_struct common = {.userdata = &s, .global_request_function = global_request};
    ssh_callbacks_init(&common);
    if (ssh_set_callbacks(session, &common) != SSH_OK) goto done;
    if (ssh_set_server_callbacks(session, &cb) != SSH_OK) goto done;
    ssh_set_auth_methods(session, SSH_AUTH_METHOD_PUBLICKEY);
    long timeout = SETUP_MS / 1000;
    if (ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &timeout) != SSH_OK) goto done;
    ssh_set_blocking(session, 1);
    if (ssh_handle_key_exchange(session) != SSH_OK) { end = "kex"; goto done; }
    diagnostic(&s, KVM_SSH_DIAG_KEX, NULL);
    ssh_set_blocking(session, 0);
    if (ssh_event_add_session(event, session) != SSH_OK) { end = "event_setup"; goto done; }
    end = "peer";
    while (ssh_is_connected(session) && !s.failed) {
        if (now_ms() >= s.deadline) { end = "deadline"; break; }
        if (s.granted && !ops->active(ctx)) { end = "revoked"; break; }
        if (s.eof) {
            ok = s.granted && s.used == 0;
            end = ok ? "ok" : "incomplete";
            break;
        }
        if (s.channel && ssh_channel_is_closed(s.channel)) { end = "channel"; break; }
        if (ssh_event_dopoll(event, POLL_MS) == SSH_ERROR) { end = "event"; break; }
    }
    if (s.failed) end = s.requested && !s.granted ? "approval" : "rejected";
done:
    completed_stream = ok;
    s.failed = true; // No new input or approval while draining the channel close handshake.
    if (s.requested) ops->release(ctx);
    if (s.channel) {
        if (ssh_channel_is_open(s.channel)) {
            long close_seconds = 1;
            ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &close_seconds);
            ssh_set_blocking(session, 1);
            if (ssh_channel_request_send_exit_status(s.channel, ok ? 0 : 1) != SSH_OK ||
                ssh_blocking_flush(session, 1000) != SSH_OK) ok = false;
            // exit-status has no protocol acknowledgement. Give the peer time to read it
            // and close. OpenSSH may disconnect at this point without a channel CLOSE.
            ssh_channel_close(s.channel);
            uint64_t until = now_ms() + 1000;
            while (!s.remote_closed && ssh_is_connected(session) && now_ms() < until) {
                if (ssh_event_dopoll(event, POLL_MS) == SSH_ERROR) break;
            }
        } else ok = false;
        ssh_remove_channel_callbacks(s.channel, &s.channel_cb);
        ssh_channel_free(s.channel);
    }
    ssh_event_remove_session(event, session);
    ssh_event_free(event);
    // libssh retains this pointer through disconnect. Drop all stack-backed callbacks.
    static struct ssh_server_callbacks_struct inactive = {.size = sizeof(inactive)};
    ssh_set_server_callbacks(session, &inactive);
    static struct ssh_callbacks_struct inactive_common = {.size = sizeof(inactive_common)};
    ssh_set_callbacks(session, &inactive_common);
    diagnostic(&s, KVM_SSH_DIAG_END, completed_stream && !ok ? "close" : end);
    return ok;
}
