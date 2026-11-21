// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include "remote_json.h"
bool pack_receive_request(void *ctx, uint64_t session, const char *client, const char *type,
                          const kvm_json_doc_t *doc, int payload, const char **state, uint32_t *offset);
void pack_receive_disconnect(void *ctx, uint64_t session);
