// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

#pragma once
#include "remote_json.h"
#include "pack_archive.h"

enum { PACK_WIDGETS = 128, PACK_STATES = 32, PACK_ACTIONS = 32, PACK_BINDS = 64, PACK_SOURCES = 16 };
enum { PACK_PERM_UI = 1, PACK_PERM_JOYSTICK = 2, PACK_PERM_TELEMETRY = 4, PACK_PERM_CAPTURE = 8,
       PACK_PERM_MONITOR = 16, PACK_PERM_KEYBOARD = 32, PACK_PERM_MOUSE = 64 };
typedef enum { PACK_CONTAINER, PACK_TEXT, PACK_BUTTON, PACK_SPACER, PACK_AXIS, PACK_SLIDER,
               PACK_GAUGE_ARC, PACK_GAUGE_NEEDLE, PACK_HORIZON, PACK_VALUE, PACK_STREAM,
               PACK_TOUCH, PACK_DIAL, PACK_TABS, PACK_CHIP, PACK_CHART, PACK_AUTOPILOT, PACK_RADIO,
               PACK_RATE_SLIDER, PACK_MONITOR, PACK_KEYBOARD, PACK_TOUCHPAD } pack_widget_kind_t;
typedef enum { PACK_TAP, PACK_CHANGE, PACK_PRESS, PACK_RELEASE, PACK_DRAG, PACK_ROTATE, PACK_LONG_PRESS, PACK_STEP_UP, PACK_STEP_DOWN } pack_event_t;
typedef enum { PACK_STATE, PACK_AXES, PACK_BUTTON_BIT, PACK_START_STREAM, PACK_STOP_STREAM, PACK_HAT } pack_action_kind_t;
typedef enum { PACK_BIND_TEXT, PACK_BIND_VALUE, PACK_BIND_PITCH, PACK_BIND_ROLL, PACK_BIND_SELECTED } pack_field_t;
enum { PACK_X = 1, PACK_Y = 2, PACK_Z = 4, PACK_RZ = 8 };

typedef struct {
    char id[49], text[97], unit[17];
    int16_t x, y, w, h, parent;
    int32_t min, max;
    pack_widget_kind_t kind;
    int16_t initial;
    uint8_t tab_first, tab_count, chip_style;
    uint8_t dead_zone_percent;
    bool spring_center, invert_y, cubic;
    uint8_t icon_member; // zero unless Button.props.icon resolved to an archive member
} pack_widget_t;
typedef struct pack_icon {
    struct pack_icon *next;
    uint8_t width, height, member, reserved;
    uint8_t bgra[]; // straight alpha, directly usable as little-endian LVGL ARGB8888
} pack_icon_t;
enum { PACK_CHARTS = 3, PACK_CHART_WIDTH = 600, PACK_CHART_HEIGHT = 440 };
typedef struct {
    char title[49];
    uint8_t member;
    uint16_t width, height;
    uint32_t palette[16];
    uint8_t *runs;
    size_t run_count;
} pack_chart_t;
typedef enum { PACK_STRING, PACK_INTEGER, PACK_FLOAT, PACK_BOOLEAN } pack_state_kind_t;
// Strings hold up to 96 UTF-8 bytes. Numeric state uses signed 64-bit integers or finite binary64.
typedef union { char text[97]; int64_t integer; double real; bool boolean; } pack_value_t;
typedef union { int64_t integer; double real; } pack_bound_t;
typedef struct {
    char id[97];
    pack_state_kind_t kind;
    pack_value_t value;
    pack_bound_t min, max;
} pack_state_t;
typedef struct { char id[49]; unsigned channel, max_age_ms; } pack_source_t;
typedef enum { PACK_IDENTITY, PACK_CLAMP, PACK_SCALE, PACK_OFFSET, PACK_ROUND, PACK_ENUM_MAP,
               PACK_HYSTERESIS, PACK_LOWPASS, PACK_FORMAT_NUMBER, PACK_BOOLEAN_THRESHOLD } pack_transform_t;
enum { PACK_ENUM_ENTRIES=64 };
typedef struct { int32_t code; char label[49]; } pack_enum_entry_t;
typedef struct {
    uint16_t source, widget;
    pack_field_t field;
    pack_transform_t transform;
    pack_state_kind_t input, output;
    double a, b, previous;
    uint32_t tick;
    uint8_t enum_start, enum_count;
    bool local, seeded;
} pack_binding_t;
typedef enum { PACK_LITERAL, PACK_EVENT_VALUE, PACK_EVENT_X, PACK_EVENT_Y, PACK_MODE_STATE, PACK_ADJUST } pack_argument_t;
typedef struct {
    char id[97];
    pack_argument_t argument;
    pack_value_t value;
    pack_event_t event;
    pack_action_kind_t kind;
    uint16_t widget, state, mask;
    bool down, defined;
} pack_action_t;
typedef struct {
    char id[101], title[97], version[32], entry[49];
    bool publisher_verified, composite;
    uint8_t tabs;
    uint8_t portrait:1, screen_count:4, min_firmware_1_4:1;
    uint8_t publisher_key_sha256[32]; // SHA-256 of the verified raw Ed25519 public key
    unsigned widgets, states, bindings, actions, sources, events_per_second, permissions, required_permissions, max_memory_kib;
    pack_icon_t *icons;
    pack_chart_t charts[PACK_CHARTS];
    struct { char label[25]; int16_t page; } tab[32];
    pack_widget_t widget[PACK_WIDGETS];
    pack_state_t state[PACK_STATES];
    pack_binding_t binding[PACK_BINDS];
    unsigned enum_entries;
    pack_enum_entry_t enum_table[PACK_ENUM_ENTRIES];
    pack_action_t action[PACK_ACTIONS];
    pack_source_t source[PACK_SOURCES];
} pack_scene_t;

static inline const pack_chart_t *pack_chart_at(const pack_scene_t *s, unsigned index)
{
    return index < PACK_CHARTS && s->charts[index].runs ? &s->charts[index] : NULL;
}

bool pack_profile_id_valid(const char *id);
bool pack_scene_manifest(pack_scene_t *scene, const kvm_json_doc_t *doc);
bool pack_scene_ui(pack_scene_t *scene, const kvm_json_doc_t *doc);
bool pack_scene_ui_assets(pack_scene_t *scene, const kvm_json_doc_t *doc, const pack_archive_t *archive);
bool pack_scene_bindings(pack_scene_t *scene, const kvm_json_doc_t *doc);
const pack_action_t *pack_scene_action(const pack_scene_t *scene, unsigned widget, pack_event_t event);
const char *pack_scene_capture_mode(const pack_scene_t *scene, const pack_action_t *action);
bool pack_scene_set(pack_scene_t *scene, const pack_action_t *action, int value, int x, int y);
bool pack_scene_tap(pack_scene_t *scene, unsigned widget);
const char *pack_value_text(pack_state_kind_t kind, const pack_value_t *value, char scratch[32]);
const char *pack_state_text(const pack_state_t *state, char scratch[32]);
bool pack_binding_apply(pack_scene_t *scene, pack_binding_t *binding, const pack_value_t *input,
                        uint32_t tick, pack_value_t *output);
