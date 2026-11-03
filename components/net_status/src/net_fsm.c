// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Wi-Fi STA link state machine (TASK-080). See net_fsm.h.
#include "net_fsm.h"

#include <string.h>

static const char *const k_names[NET_LINK_COUNT] = {"off",       "not_configured", "connecting_cached", "connecting",
                                                    "associated", "connected",      "backoff"};

const char *net_link_name(net_link_t l)
{
    return (unsigned)l < NET_LINK_COUNT ? k_names[l] : "?";
}

void net_fsm_init(net_fsm_t *f, const net_fsm_cfg_t *cfg, net_rand_fn rnd, void *rnd_ctx)
{
    memset(f, 0, sizeof(*f));
    const net_fsm_cfg_t def = NET_FSM_CFG_DEFAULT;
    f->cfg = cfg != NULL ? *cfg : def;
    if (f->cfg.backoff_min_us == 0) {
        f->cfg.backoff_min_us = 1;
    }
    if (f->cfg.backoff_max_us < f->cfg.backoff_min_us) {
        f->cfg.backoff_max_us = f->cfg.backoff_min_us;
    }
    f->rnd = rnd;
    f->rnd_ctx = rnd_ctx;
    f->link = NET_LINK_OFF;
}

static void act(net_actions_t *out, net_act_kind_t k, const net_cache_t *c)
{
    if (out->n < NET_MAX_ACTIONS) {
        out->a[out->n].kind = k;
        if (c != NULL) {
            out->a[out->n].cache = *c;
        } else {
            memset(&out->a[out->n].cache, 0, sizeof(out->a[out->n].cache));
        }
        out->n++;
    }
}

static bool cache_ok(const net_cache_t *c)
{
    return c->valid && c->channel >= 1 && c->channel <= 14;
}

uint32_t net_fsm_backoff_us(const net_fsm_t *f, uint32_t failures)
{
    uint64_t d = f->cfg.backoff_min_us;
    for (uint32_t i = 1; i < failures && d < f->cfg.backoff_max_us; i++) {
        d *= 2;
    }
    if (d > f->cfg.backoff_max_us) {
        d = f->cfg.backoff_max_us;
    }
    if (f->rnd != NULL && d >= 4) {
        d += f->rnd(f->rnd_ctx) % (uint32_t)(d / 4); // up to +25 %: reconnecting devices do not synchronise
    }
    return (uint32_t)d;
}

static void go_connect_full(net_fsm_t *f, uint64_t now, net_actions_t *out)
{
    f->link = NET_LINK_CONNECTING;
    f->deadline_us = now + f->cfg.connect_timeout_us;
    f->st.attempts_full++;
    act(out, NET_ACT_CONNECT_FULL, NULL);
}

static void go_connect(net_fsm_t *f, uint64_t now, net_actions_t *out)
{
    if (cache_ok(&f->cache)) {
        f->link = NET_LINK_CONNECTING_CACHED;
        f->deadline_us = now + f->cfg.cached_timeout_us;
        f->st.attempts_cached++;
        act(out, NET_ACT_CONNECT_CACHED, &f->cache);
    } else {
        go_connect_full(f, now, out);
    }
}

static void go_backoff(net_fsm_t *f, uint64_t now)
{
    f->failures++;
    f->link = NET_LINK_BACKOFF;
    f->deadline_us = now + net_fsm_backoff_us(f, f->failures);
}

// The cached attempt failed: forget the record and go straight to a full connect (ADR-017, AT-EXT-02).
static void cached_fallback(net_fsm_t *f, uint64_t now, net_actions_t *out)
{
    f->st.cached_fallbacks++;
    memset(&f->cache, 0, sizeof(f->cache));
    act(out, NET_ACT_CACHE_INVALIDATE, NULL);
    go_connect_full(f, now, out);
}

// Abort the running attempt before starting another one: the driver reports the abort as a disconnect.
static void abort_for_retry(net_fsm_t *f, net_actions_t *out)
{
    act(out, NET_ACT_DISCONNECT, NULL);
    f->echo_pending = true;
}

static void stop_attempt(net_fsm_t *f)
{
    f->deadline_us = 0;
    memset(&f->pending, 0, sizeof(f->pending));
}

void net_fsm_handle(net_fsm_t *f, const net_event_t *ev, net_actions_t *out)
{
    out->n = 0;
    const uint64_t now = ev->now_us;
    const net_link_t s = f->link;

    switch (ev->kind) {
    case NET_EV_START:
        if (s != NET_LINK_OFF) {
            f->st.ignored++;
            return;
        }
        f->configured = ev->has_credentials;
        f->cache = cache_ok(&ev->cache) ? ev->cache : (net_cache_t){0};
        f->failures = 0;
        if (!f->configured) {
            f->link = NET_LINK_NOT_CONFIGURED;
            f->deadline_us = 0;
            return;
        }
        go_connect(f, now, out);
        return;

    case NET_EV_STOP:
        if (s == NET_LINK_OFF) {
            f->st.ignored++;
            return;
        }
        if (s != NET_LINK_NOT_CONFIGURED && s != NET_LINK_BACKOFF) {
            act(out, NET_ACT_DISCONNECT, NULL);
        }
        act(out, NET_ACT_RADIO_STOP, NULL);
        stop_attempt(f);
        f->echo_pending = false;
        f->link = NET_LINK_OFF;
        return;

    case NET_EV_CREDENTIALS_SET:
        if (s == NET_LINK_OFF) {
            f->configured = true; // applied on the next START
            return;
        }
        if (s != NET_LINK_NOT_CONFIGURED && s != NET_LINK_BACKOFF) {
            abort_for_retry(f, out);
        }
        stop_attempt(f);
        f->configured = true;
        f->failures = 0;
        memset(&f->cache, 0, sizeof(f->cache)); // another network: the old BSSID means nothing
        act(out, NET_ACT_CACHE_INVALIDATE, NULL);
        go_connect_full(f, now, out);
        return;

    case NET_EV_CREDENTIALS_CLEARED:
        if (s != NET_LINK_OFF && s != NET_LINK_NOT_CONFIGURED && s != NET_LINK_BACKOFF) {
            act(out, NET_ACT_DISCONNECT, NULL);
        }
        stop_attempt(f);
        f->configured = false;
        f->failures = 0;
        memset(&f->cache, 0, sizeof(f->cache));
        act(out, NET_ACT_CACHE_INVALIDATE, NULL);
        if (s != NET_LINK_OFF) {
            f->link = NET_LINK_NOT_CONFIGURED;
        }
        return;

    case NET_EV_ASSOCIATED:
        if (s != NET_LINK_CONNECTING_CACHED && s != NET_LINK_CONNECTING) {
            f->st.ignored++;
            return;
        }
        f->echo_pending = false; // the new attempt is through: any later disconnect is real
        memcpy(f->pending.bssid, ev->bssid, sizeof(f->pending.bssid));
        f->pending.channel = ev->channel;
        f->pending.valid = ev->channel >= 1 && ev->channel <= 14;
        f->channel = ev->channel;
        f->link = NET_LINK_ASSOCIATED;
        f->deadline_us = now + f->cfg.dhcp_timeout_us;
        return;

    case NET_EV_GOT_IP:
        if (s != NET_LINK_ASSOCIATED) {
            f->st.ignored++;
            return;
        }
        f->link = NET_LINK_CONNECTED;
        f->deadline_us = 0;
        f->failures = 0;
        f->st.connects++;
        if (f->pending.valid) { // TECH-071: only a connection that reached an IP is worth caching
            const bool same = cache_ok(&f->cache) && f->cache.channel == f->pending.channel &&
                              memcmp(f->cache.bssid, f->pending.bssid, sizeof(f->cache.bssid)) == 0;
            f->cache = f->pending;
            if (!same) {
                act(out, NET_ACT_CACHE_STORE, &f->cache); // unchanged record: no flash write
            }
        }
        return;

    case NET_EV_LOST_IP:
        if (s != NET_LINK_CONNECTED) {
            f->st.ignored++;
            return;
        }
        f->link = NET_LINK_ASSOCIATED; // still associated: DHCP gets its time limit again
        f->deadline_us = now + f->cfg.dhcp_timeout_us;
        return;

    case NET_EV_DISCONNECTED:
        f->last_reason = ev->reason;
        if (s == NET_LINK_OFF || s == NET_LINK_NOT_CONFIGURED || s == NET_LINK_BACKOFF) {
            f->st.ignored++; // e.g. the echo of our own DISCONNECT
            return;
        }
        if (f->echo_pending && (s == NET_LINK_CONNECTING || s == NET_LINK_CONNECTING_CACHED)) {
            f->echo_pending = false; // the abort we asked for, not a failure of the new attempt
            f->st.echoes++;
            return;
        }
        f->echo_pending = false;
        f->st.disconnects++;
        stop_attempt(f);
        if (s == NET_LINK_CONNECTING_CACHED) {
            cached_fallback(f, now, out);
        } else if (s == NET_LINK_CONNECTED) {
            go_connect(f, now, out); // the link was good: retry at once, cached first (ADR-017)
        } else {
            go_backoff(f, now);
        }
        return;

    case NET_EV_TICK:
        if (f->deadline_us == 0 || now < f->deadline_us) {
            return;
        }
        switch (s) {
        case NET_LINK_CONNECTING_CACHED:
            f->st.timeouts++;
            stop_attempt(f);
            abort_for_retry(f, out);
            cached_fallback(f, now, out);
            break;
        case NET_LINK_CONNECTING:
        case NET_LINK_ASSOCIATED:
            f->st.timeouts++;
            stop_attempt(f);
            act(out, NET_ACT_DISCONNECT, NULL);
            go_backoff(f, now);
            break;
        case NET_LINK_BACKOFF:
            f->deadline_us = 0;
            go_connect(f, now, out);
            break;
        default:
            f->deadline_us = 0;
            break;
        }
        return;
    }
    f->st.ignored++;
}

void net_fsm_status(const net_fsm_t *f, net_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->link = f->link;
    out->ip_present = f->link == NET_LINK_CONNECTED;
    out->configured = f->configured;
    out->channel = f->link == NET_LINK_OFF ? 0 : f->channel;
    out->last_reason = f->last_reason;
    out->consecutive_failures = f->failures;
    out->backoff_until_us = f->link == NET_LINK_BACKOFF ? f->deadline_us : 0;
    out->st = f->st;
}
