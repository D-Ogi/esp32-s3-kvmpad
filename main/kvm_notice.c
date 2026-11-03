// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "kvm_notice.h"

#include "esp_attr.h"

// TECH-084: retained in .rodata of every image (`strings` on the .bin finds it); `used` keeps the
// linker from dropping it even if no code referenced it.
__attribute__((used)) static const char k_notice[] =
    "KVMpad firmware | SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 | "
    "Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)";

const char *kvm_notice(void)
{
    return k_notice;
}
