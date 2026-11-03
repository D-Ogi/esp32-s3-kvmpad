# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#
# remote_protocol (TASK-100, protocol part): CLIENT protocol v1 envelope reader, validator and
# session layer (spec 03 §6, §7). Every source file is pure C with no ESP-IDF dependency and is
# built by the host suite in test/remote_protocol; hid_core is needed for the intent, owner and
# release-reason types and for the chord allowlist. No TLS, no WebSocket, no TinyUSB here.
idf_component_register(
    SRCS "src/remote_json.c" "src/remote_wire.c" "src/remote_protocol.c"
    INCLUDE_DIRS "include"
    REQUIRES hid_core)
