// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "pack_keyboard_surface.h"
#include "ui_keyboard_surface.h"
#include "app_config.h"
#include "hid_dispatch.h"

typedef struct package_keyboard {
    struct package_keyboard *next;
    lv_obj_t *surface;
    bool (*ready)(void);
} package_keyboard_t;
static package_keyboard_t *keyboards;

static bool ready_cb(void *ctx) { return ((package_keyboard_t *)ctx)->ready(); }

static bool edge_cb(void *ctx,bool down,uint8_t usage)
{
    (void)ctx;
    kvm_hid_intent_t intent={.type=down ? KVM_HID_KEY_DOWN : KVM_HID_KEY_UP,.data.key={.usage_id=usage}};
    return hid_dispatch_local(&intent)==ESP_OK;
}

static void release_cb(void *ctx)
{
    (void)ctx;
    hid_dispatch_release_all(KVM_RA_GESTURE_CANCEL);
}

static void forget(lv_event_t *event)
{
    package_keyboard_t *k=lv_event_get_user_data(event);
    package_keyboard_t **link=&keyboards;
    while (*link && *link!=k) link=&(*link)->next;
    if (*link) *link=k->next;
    lv_free(k);
}

void pack_keyboard_surface_release(void)
{
    for (package_keyboard_t *k=keyboards;k;k=k->next) ui_keyboard_surface_cancel(k->surface);
}

bool pack_keyboard_surface_buffered_target(lv_obj_t *target)
{
    return ui_keyboard_surface_buffered_target(target);
}

void pack_keyboard_surface_set_caps(lv_obj_t *surface,bool known,bool on)
{
    ui_keyboard_surface_set_caps(surface,known,on);
}

lv_obj_t *pack_keyboard_surface_open(lv_obj_t *parent,int width,int height,bool (*ready)(void))
{
    if (!parent || !ready) return NULL;
    char id[KVM_CONFIG_LAYOUT_ID_CAP];
    app_config_host_layout(id,sizeof(id));
    const kbd_layout_t *layout=kbd_layouts_find(id);
    if (!layout) layout=kbd_layouts_find("us");
    package_keyboard_t *k=lv_malloc_zeroed(sizeof(*k));
    if (!k) return NULL;
    k->ready=ready;
    ui_keyboard_surface_hooks_t hooks={.ready=ready_cb,.edge=edge_cb,.release_all=release_cb,.ctx=k};
    k->surface=ui_keyboard_surface_create(parent,width,height,layout,&hooks);
    if (!k->surface || !lv_obj_add_event_cb(k->surface,forget,LV_EVENT_DELETE,k)) {
        if (k->surface) lv_obj_delete(k->surface);
        lv_free(k); return NULL;
    }
    // The authored rectangle is the entire usable keyboard, including its edges.
    lv_obj_set_style_pad_all(parent,0,LV_PART_MAIN);
    lv_obj_set_style_border_width(parent,0,LV_PART_MAIN);
    lv_obj_set_scrollable(parent,false);
    k->next=keyboards; keyboards=k;
    return k->surface;
}
