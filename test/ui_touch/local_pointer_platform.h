// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include "../live_view/hid_test_platform.h"

enum { pdPASS = 1 };
QueueHandle_t xQueueCreate(unsigned capacity, unsigned width);
int xTaskCreatePinnedToCore(void (*entry)(void *), const char *name, unsigned stack, void *arg,
                            unsigned priority, void *handle, unsigned core);
bool pointer_queue_receive(QueueHandle_t q, void *item, unsigned wait);
bool pointer_queue_send(QueueHandle_t q, const void *item, unsigned wait);
#ifdef KVM_TEST_BRIDGE_TASK
#define xQueueReceive pointer_queue_receive
#define xQueueSend pointer_queue_send
#endif
