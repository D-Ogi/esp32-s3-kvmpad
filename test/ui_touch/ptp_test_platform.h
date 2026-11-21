// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include "../live_view/hid_test_platform.h"

enum { pdPASS = 1, LV_INDEV_TYPE_POINTER, LV_INDEV_STATE_PRESSED, LV_INDEV_STATE_RELEASED };
typedef int lv_indev_t;
typedef int lv_display_t;
typedef struct _lv_obj_t lv_obj_t;
typedef struct { int32_t x,y; } lv_point_t;
typedef struct { lv_point_t point; int state; bool continue_reading; } lv_indev_data_t;
// This transport-only harness has no controls eligible for buffered presses.
static inline lv_display_t *lv_indev_get_display(lv_indev_t *d) { (void)d; return NULL; }
static inline void lv_display_rotate_point(lv_display_t *d,lv_point_t *p) { (void)d; (void)p; }
static inline lv_obj_t *lv_display_get_layer_sys(lv_display_t *d) { (void)d; return NULL; }
static inline lv_obj_t *lv_display_get_layer_top(lv_display_t *d) { (void)d; return NULL; }
static inline lv_obj_t *lv_display_get_screen_active(lv_display_t *d) { (void)d; return NULL; }
static inline lv_obj_t *lv_indev_search_obj(lv_obj_t *root,lv_point_t *p) { (void)root; (void)p; return NULL; }
typedef void (*pointer_reader_t)(lv_indev_t *, lv_indev_data_t *);
lv_indev_t *lv_indev_create(void);
void lv_indev_set_read_cb(lv_indev_t *, pointer_reader_t);
static inline void lv_indev_set_type(lv_indev_t *d, int type) { (void)d; (void)type; }
static inline void lv_indev_set_display(lv_indev_t *d, void *display) { (void)d; (void)display; }
static inline void lv_indev_reset(lv_indev_t *d, void *obj) { (void)d; (void)obj; }
static inline bool ui_platform_is_running(void) { return true; }
static inline bool ui_platform_lock(unsigned ms) { (void)ms; return true; }
static inline void ui_platform_unlock(void) {}
static inline void *ui_platform_display(void) { return NULL; }
uint32_t esp_random(void);
QueueHandle_t xQueueCreate(unsigned cap, unsigned width);
int xTaskCreatePinnedToCore(void (*entry)(void *), const char *, unsigned, void *, unsigned, void *, unsigned);
bool test_bridge_receive(QueueHandle_t q, void *msg, unsigned wait);
bool test_bridge_send(QueueHandle_t q, const void *msg, unsigned wait);
#ifdef KVM_TEST_BRIDGE_TASK
#define xQueueReceive test_bridge_receive
#define xQueueSend test_bridge_send
#endif
