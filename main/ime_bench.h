// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// `ime [status | mount | bench]`: TASK-072 footprint spike on the board (see ime_bench.c). No HID.
#pragma once

int ime_bench_console_cmd(int argc, char **argv);
