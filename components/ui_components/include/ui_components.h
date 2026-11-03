// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Umbrella header of the shared product components (TASK-060D, 07 §7). A view that needs one
// component should include that component's header; this one exists for the shell, which needs all
// of them.
//
// Dependency rule (07 §2): ui_views -> ui_components -> ui_theme. No TinyUSB, no network, no
// pairing logic anywhere below this header (AGENTS-UI MUST 4). The components raise typed control
// ids; only the view turns a control id into a UI action for the InputArbiter.
#pragma once

#include "ui_action_key.h"
#include "ui_modal.h"
#include "ui_segmented.h"
#include "ui_status_item.h"
#include "ui_theme.h"
#include "ui_toast.h"
#include "ui_tokens.h"
#include "ui_topbar.h"
