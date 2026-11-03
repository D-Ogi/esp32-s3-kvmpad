// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once

#include <stdint.h>

/** License notice embedded in the image (TECH-084): SPDX id + Required Notice. */
const char *kvm_notice(void);

/** Boot counter kept in NVS (100x boot test, TASK-010). */
uint32_t kvm_boot_count(void);
