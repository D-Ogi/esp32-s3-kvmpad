// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Router and root shell (TASK-060C, 07 §5): one activity at a time. The shell always exists (top bar slot,
// content slot, overlay layer, toast layer); primary views are created lazily on their first show and stay
// cached (at most UI_VIEW_COUNT, 07 §5 "at most three main views"), so navigation allocates nothing after
// the first visit. Every function runs in the LVGL owner context (AGENTS-UI §3) and never blocks.
//
// Atomic mode switch (07 §5, AGENTS-UI §6): cancel_input (touch cancel + HID release request) -> old view
// on_leave -> hide old -> create/show new -> on_enter -> publish active_view -> input_area_changed. The router
// never assumes the asynchronous HID release succeeded: it counts refusals, and the HID core stays the
// safety owner (RELEASE_ALL on every loss).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "ui_contracts.h"
#include "ui_store.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_OVERLAY_STACK_MAX 2  // a modal over a view, plus one confirmation over the modal; no deeper nesting
#define UI_OVERLAY_NONE 0u

typedef enum {
    UI_CANCEL_VIEW_CHANGE = 0, // primary view switch
    UI_CANCEL_OVERLAY_OPEN,    // a blocking overlay captures the input below it
    UI_CANCEL_SHUTDOWN,        // router deinit
} ui_cancel_reason_t;

/** Primary view (MOUSE / KEYBOARD / SHORTCUTS). Callbacks run in the LVGL owner context. */
typedef struct {
    ui_view_id_t id;
    /** Build the view into `slot` and return its root object (a child of `slot`); NULL = failed. Called once. */
    lv_obj_t *(*create)(void *ctx, lv_obj_t *slot);
    void (*on_enter)(void *ctx);
    /** Cancel local gesture/press state; the router has already requested the domain release. */
    void (*on_leave)(void *ctx);
    /** The root object is deleted by the router right after this returns (observers detach with it). */
    void (*destroy)(void *ctx);
    /** Optional: the touchpad object of this view (PAD routing, 07 §6); NULL if the view has no pad. */
    lv_obj_t *(*pad_obj)(void *ctx);
    void *ctx;
} ui_view_descriptor_t;

/** Overlay (modal, details, settings, confirmation). */
typedef struct {
    uint16_t id; // != UI_OVERLAY_NONE
    /** Build the overlay content into `card` (a child of the input-absorbing scrim). False = failed. */
    bool (*create)(void *ctx, lv_obj_t *card);
    /** Called after the overlay is removed (its objects are already deleted). */
    void (*on_close)(void *ctx);
    bool blocking; // true: captures all input below it (scrim); false: a non-modal panel
    void *ctx;
} ui_overlay_descriptor_t;

typedef struct {
    /** Touch cancel + HID release request. Returns false when the request was refused (counted, not fatal). */
    bool (*cancel_input)(void *ctx, ui_cancel_reason_t why);
    /** The pad availability or geometry may have changed: re-read ui_router_pad_area(). */
    void (*input_area_changed)(void *ctx);
    void *ctx;
} ui_nav_hooks_t;

typedef struct {
    int32_t width, height; // screen size
    int32_t topbar_h;      // top bar slot height (the content slot is below it)
    /**
     * Tint of the full-screen scrim under a blocking overlay. ui_core owns no design tokens (07 §2), so the
     * shell passes the theme's scrim here and every blocking surface - router overlay and UiModal alike -
     * dims the screen by the same amount. `scrim_opa` 0 selects the built-in fallback (black, LV_OPA_50).
     */
    uint32_t scrim_color; // 0xRRGGBB
    uint8_t scrim_opa;    // 0 = default
} ui_shell_layout_t;

typedef struct {
    uint32_t shows;            // completed view switches
    uint32_t show_noop;        // show of the already active view
    uint32_t show_refused;     // unknown id, re-entrant call, blocking overlay open, create failed
    uint32_t views_created;
    uint32_t cancel_requests;
    uint32_t cancel_refused;   // cancel_input returned false
    uint32_t overlays_shown;
    uint32_t overlays_dismissed;
    uint32_t overlay_refused;  // unknown id, duplicate, stack full, re-entrant, create failed
} ui_router_counters_t;

typedef struct {
    lv_obj_t *root;            // the shell screen (or the parent given to init)
    lv_obj_t *topbar_slot;
    lv_obj_t *content_slot;
    lv_obj_t *overlay_layer;   // hidden while the overlay stack is empty, so it never catches input
    lv_obj_t *toast_layer;     // never clickable: toasts do not block the pad
    ui_store_t *store;         // may be NULL (tests); receives active_view / overlay
    ui_nav_hooks_t hooks;
    const ui_view_descriptor_t *views[UI_VIEW_COUNT];
    lv_obj_t *view_root[UI_VIEW_COUNT];
    const ui_overlay_descriptor_t *overlays;
    uint8_t overlay_count;
    struct {
        const ui_overlay_descriptor_t *d;
        lv_obj_t *scrim;
    } stack[UI_OVERLAY_STACK_MAX];
    uint8_t depth;
    uint32_t scrim_color;      // resolved from ui_shell_layout_t at init
    uint8_t scrim_opa;
    int16_t active;            // ui_view_id_t, -1 = none yet
    bool busy;                 // inside a switch: re-entrant calls are refused
    bool ready;
    ui_router_counters_t c;
} ui_router_t;

/**
 * Create the shell on `parent` (normally a fresh screen; it is not loaded here). `views` holds `n_views`
 * descriptors with distinct ids < UI_VIEW_COUNT, except UI_VIEW_SETTINGS (an overlay, 07 §5). Descriptor
 * arrays must outlive the router. Returns false on invalid arguments or allocation failure (nothing kept).
 */
bool ui_router_init(ui_router_t *r, lv_obj_t *parent, const ui_shell_layout_t *layout, ui_store_t *store,
                    const ui_view_descriptor_t *views, uint8_t n_views, const ui_overlay_descriptor_t *overlays,
                    uint8_t n_overlays, const ui_nav_hooks_t *hooks);

/** Dismiss every overlay, leave the active view, destroy all views and delete the shell objects. */
void ui_router_deinit(ui_router_t *r);

/** Switch the primary view (atomic sequence above). True when `target` is active afterwards. */
bool ui_router_show(ui_router_t *r, ui_view_id_t target);

/** Active view id, or -1 before the first show. */
int ui_router_active(const ui_router_t *r);

/**
 * Screen rectangle of the active view's pad, when the pad may receive touches now: the view has a pad and no
 * blocking overlay is open. False otherwise (the touch router then routes nothing to the pad).
 */
bool ui_router_pad_area(ui_router_t *r, lv_area_t *out);

/** Overlay functions: ui_overlay.h. */

#ifdef __cplusplus
}
#endif
