// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "pack_receive.h"
#include "pack_receive_wire.h"
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
#define LOCK() portENTER_CRITICAL(&lock)
#define UNLOCK() portEXIT_CRITICAL(&lock)
static uint64_t now_ms(void) { return (uint64_t)esp_timer_get_time() / 1000; }
#else
// Native deterministic harness supplies the clock. Production always uses the
// RTOS guard.
extern uint64_t pack_receive_test_time(void);
#define now_ms pack_receive_test_time
#define LOCK() ((void)0)
#define UNLOCK() ((void)0)
#endif

typedef enum {
    CLOSED,
    ARMED,
    PENDING,
    READY,
    REVIEW,
    COMMITTING,
    INSTALLED,
    DENIED,
    CANCELLED,
    EXPIRED,
    FAILED
} state_t;
static const char *const names[] = {"closed",    "armed",      "pending",   "ready",
                                    "review",    "committing", "installed", "denied",
                                    "cancelled", "expired",    "failed"};
static struct {
    uint32_t epoch, size, offset;
    uint64_t session, deadline, absolute;
    state_t phase;
    bool delivered, abort_notice;
    pack_receive_part_t part;
} rx;
static struct {
    uint64_t session;
    uint32_t offset;
    state_t phase;
} last;

static void terminal(state_t why)
{
    rx.phase = why;
    rx.abort_notice = why != INSTALLED;
    rx.delivered = false;
    memset(&rx.part, 0, sizeof(rx.part));
    if (rx.session) {
        last.session = rx.session;
        last.offset = rx.offset;
        last.phase = why;
    }
}

static void expire(uint64_t t)
{
    if (rx.phase >= ARMED && rx.phase <= REVIEW &&
        (t >= rx.deadline || (rx.phase != REVIEW && t >= rx.absolute)))
        terminal(EXPIRED);
}

uint32_t pack_receive_arm(void)
{
    uint64_t t = now_ms();
    LOCK();
    expire(t);
    uint32_t epoch = 0;
    if (rx.phase == CLOSED || rx.phase >= INSTALLED) {
        epoch = rx.epoch + 1;
        if (!epoch)
            epoch++;
        memset(&rx, 0, sizeof(rx));
        rx.epoch = epoch;
        rx.phase = ARMED;
        rx.deadline = rx.absolute = t + 60000;
    }
    UNLOCK();
    return epoch;
}

bool pack_receive_take(uint32_t epoch, pack_receive_part_t *out)
{
    if (!epoch || !out)
        return false;
    uint64_t t = now_ms();
    LOCK();
    expire(t);
    bool available = false;
    if (epoch != rx.epoch || rx.abort_notice) {
        if (epoch == rx.epoch)
            rx.abort_notice = false;
        memset(out, 0, sizeof(*out));
        out->kind = PACK_RECEIVE_ABORT;
        available = true;
    } else if (rx.phase == PENDING && !rx.delivered) {
        *out = rx.part;
        rx.delivered = true;
        available = true;
    }
    UNLOCK();
    return available;
}

void pack_receive_ack(uint32_t epoch, bool ok)
{
    uint64_t t = now_ms();
    LOCK();
    expire(t);
    if (epoch == rx.epoch && rx.phase == PENDING && rx.delivered) {
        if (!ok)
            terminal(FAILED);
        else {
            if (rx.part.kind == PACK_RECEIVE_DATA)
                rx.offset += rx.part.len;
            rx.phase = rx.part.kind == PACK_RECEIVE_END ? REVIEW : READY;
            rx.deadline = t + (rx.phase == REVIEW ? 90000 : 30000);
            memset(&rx.part, 0, sizeof(rx.part));
            rx.delivered = false;
        }
    }
    UNLOCK();
}

bool pack_receive_review_active(uint32_t epoch)
{
    uint64_t t = now_ms();
    LOCK();
    expire(t);
    bool active = epoch == rx.epoch && rx.phase == REVIEW;
    UNLOCK();
    return active;
}

bool pack_receive_begin_commit(uint32_t epoch)
{
    uint64_t t = now_ms();
    LOCK();
    expire(t);
    bool active = epoch == rx.epoch && rx.phase == REVIEW;
    if (active)
        rx.phase = COMMITTING;
    UNLOCK();
    return active;
}

void pack_receive_finish(uint32_t epoch, bool installed)
{
    uint64_t t = now_ms();
    LOCK();
    expire(t);
    if (epoch == rx.epoch) {
        if (rx.phase == COMMITTING)
            terminal(installed ? INSTALLED : FAILED);
        else if (rx.phase == REVIEW && !installed)
            terminal(DENIED);
    }
    UNLOCK();
}

void pack_receive_cancel(uint32_t epoch)
{
    LOCK();
    if (epoch == rx.epoch && rx.phase >= ARMED && rx.phase <= REVIEW)
        terminal(CANCELLED);
    UNLOCK();
}

void pack_receive_disconnect(void *ctx, uint64_t session)
{
    (void)ctx;
    LOCK();
    if (session && rx.session == session && rx.phase >= ARMED && rx.phase <= REVIEW)
        terminal(CANCELLED);
    UNLOCK();
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static unsigned hex_field(const kvm_json_doc_t *doc, int pl, const char *key, uint8_t *out,
                          unsigned max)
{
    int i = kvm_json_member(doc, pl, key);
    if (kvm_json_type(doc, i) != KVM_JSON_STRING)
        return 0;
    const kvm_json_tok_t *token = &kvm_json_tokens(doc)[i];
    unsigned count = token->len / 2;
    if (!count || count > max || token->len % 2)
        return 0;
    const char *s = doc->text + token->start;
    for (unsigned j = 0; j < count; ++j) {
        int a = hex_digit(s[2 * j]), b = hex_digit(s[2 * j + 1]);
        if (a < 0 || b < 0)
            return 0;
        out[j] = (uint8_t)(16 * a + b);
    }
    return count;
}

bool pack_receive_request(void *ctx, uint64_t session, const char *client, const char *type,
                          const kvm_json_doc_t *doc, int pl, const char **state, uint32_t *offset)
{
    (void)ctx;
    if (!session || kvm_json_type(doc, pl) != KVM_JSON_OBJECT)
        return false;
    pack_receive_part_t part = {0};
    bool poll = !strcmp(type, "pack.poll"), cancel = !strcmp(type, "pack.cancel");
    int64_t n;
    if (!strcmp(type, "pack.offer")) {
        part.kind = PACK_RECEIVE_BEGIN;
        if (kvm_json_count(doc, pl) != 2 ||
            !kvm_json_get_i64(doc, kvm_json_member(doc, pl, "size"), &n) || n < 1 || n > 262144 ||
            hex_field(doc, pl, "sha256", part.sha256, 32) != 32)
            return false;
        part.size = (uint32_t)n;
        for (unsigned i = 0; client && client[i] && i < sizeof(part.client) - 1; ++i)
            part.client[i] = client[i] >= 32 && client[i] <= 126 && client[i] != '#' ? client[i] : '?';
    } else if (!strcmp(type, "pack.data")) {
        part.kind = PACK_RECEIVE_DATA;
        if (kvm_json_count(doc, pl) != 2 ||
            !kvm_json_get_i64(doc, kvm_json_member(doc, pl, "offset"), &n) || n < 0 || n >= 262144 ||
            !(part.len = (uint16_t)hex_field(doc, pl, "hex", part.data, sizeof(part.data))))
            return false;
        part.offset = (uint32_t)n;
    } else if (poll || cancel || !strcmp(type, "pack.end")) {
        if (kvm_json_count(doc, pl))
            return false;
        part.kind = PACK_RECEIVE_END;
    } else
        return false;

    uint64_t t = now_ms();
    LOCK();
    expire(t);
    bool accepted = true;
    if (part.kind == PACK_RECEIVE_BEGIN && rx.phase == ARMED) {
        rx.session = session;
        rx.size = part.size;
        rx.absolute = t + 300000;
        rx.deadline = t + 30000;
        rx.phase = PENDING;
        rx.part = part;
    } else if (session == rx.session && rx.phase >= PENDING && rx.phase <= COMMITTING) {
        if (cancel) {
            if (rx.phase != COMMITTING)
                terminal(CANCELLED);
        } else if (!poll) {
            if (rx.phase != READY ||
                (part.kind == PACK_RECEIVE_DATA &&
                 (part.offset != rx.offset || part.len > rx.size - rx.offset)) ||
                (part.kind == PACK_RECEIVE_END && rx.offset != rx.size) ||
                part.kind == PACK_RECEIVE_BEGIN) {
                if (rx.phase != COMMITTING)
                    terminal(FAILED);
                accepted = false;
            } else {
                rx.part = part;
                rx.delivered = false;
                rx.phase = PENDING;
                rx.deadline = t + 30000;
            }
        }
    }
    if (session == rx.session) {
        *state = names[rx.phase];
        *offset = rx.offset;
    } else if (session == last.session) {
        *state = names[last.phase];
        *offset = last.offset;
    } else {
        *state = rx.session && rx.phase >= PENDING && rx.phase <= COMMITTING ? "busy" : "closed";
        *offset = 0;
    }
    UNLOCK();
    return accepted;
}
