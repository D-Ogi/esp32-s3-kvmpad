# KVMpad developer handbook

Short, practical rules for working on this firmware and its board. They come from mistakes that already happened
here. The specification and the ADRs stay normative; this page is the "how", not the "what".

## 1. The board

| Fact | Consequence |
|---|---|
| Target is the **ESP32-S3-Touch-LCD-7B** (1024×600, CH32V003 expander at 0x24, ADR-026), board #1 has 16 MiB flash and 8 MiB PSRAM | Never flash the 16 MiB partition table onto an 8 MiB unit |
| EXIO5 LOW routes the OTG port to USB | `board_hal_init()` confirms it by read-back before USB starts. Do not reorder `app_main` |
| The HID port is plugged into a HOST that someone may be using | Every reset re-enumerates USB: the HOST plays a sound and the user loses the keyboard and mouse for a moment |
| Raw console on UART0 (COM3 via CH343) | `test/hil/kvm_serial.py cmd ...` sends commands **without** resetting. Flashing resets once |

**Reset budget.**
- At most 15 resets per test series. Long loops of 100 reboots need the owner's explicit request.
- **Stop at the first unexpected reset.** If a command crashes the board once, do not send it again, and do not send
  the rest of the series. Read the log (panic reason, backtrace), fix it, and verify the fix with a single command
  before you continue.
- Group firmware changes so that one flash covers them. Do not flash for a log message alone.

**Before a new console command touches the board:**
1. Know which task runs it. Console commands run in `console_repl`, which has a 6 KiB stack.
2. Do not do heavy work there: rendering, TLS, large `printf` buffers, or deep recursion. Hand the work to the task that
   owns it, for example LVGL work through `lv_async_call()` in the LVGL task, and wait on a semaphore.
3. Run it once and read `tasks` (stack high-water marks) before you put it in a script.

**While someone uses the board:** do not run the full-screen bench scenes (`ui bench full`, `ui demo`). Full redraws
take PSRAM bandwidth from the RGB scan-out and the panel flickers. Product screens redraw small areas only.

**Never:** burn eFuses, enable secure boot or flash encryption, or run a factory reset from tooling (AGENTS.md
prohibition 6).

**Secrets:** pass Wi-Fi credentials only through environment variables to `test/hil/wifi_provision.py`. They never go
into a file, a log, evidence or a report. Logs never contain typed keys, tokens or pairing codes.

**Evidence:** raw logs and pictures go to `../evidence/` (outside the repository). Reports in `test/reports/` quote the
key lines and the SHA256 of each file. A hardware result is `PASS_HW` only when a test really ran on the board;
otherwise it is `NOT_RUN_HW`.

## 2. Memory

Measured budget (board #1, shell + Wi-Fi + USB): internal heap about 127 KiB free, about 88 KiB minimum. The spec's
soft floor under stress is 64 KiB (02 §11.2). Read it with `heap` and `tasks`, and after every change that adds a
buffer or a task.

| Rule | How |
|---|---|
| Internal RAM is for what needs it: ISR data, DMA descriptors, small queues and mutexes, task stacks, HID structures | Everything else goes to PSRAM |
| Big static buffers (≥ 1 KiB: capture rings, text lines, records) | `EXT_RAM_BSS_ATTR` (needs `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`) |
| Big dynamic buffers (snapshots, images) | `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` and free them on every path |
| Wi-Fi, lwIP, mbedTLS | Already set to allocate in PSRAM (`sdkconfig.defaults`). Wi-Fi IRAM optimizations are off on purpose. Do not turn them back on without measuring |
| Task stacks | Measure the high-water mark under load. Keep about 1 KiB of margin, not 5. Every stack lives in internal RAM |
| Hot paths (per touch sample, per HID report, per protocol message) | Zero allocation: caller-owned or static storage, fixed-size queues |
| Flash writes | Debounced and coalesced (config_store). Never write on every tap or move |

When RAM gets low, first look for: oversized stacks, diagnostic buffers in internal BSS, IRAM code, and buffers that
could be in PSRAM. Use `idf.py size --archives` and `esp_idf_size`. Moving one more subsystem to PSRAM is rarely the fix.

## 3. Modules: small, pure, testable

**One core, one adapter.** Every feature has a pure C core with no IDF, no FreeRTOS and no LVGL task assumptions,
plus a thin platform adapter.

| Core (host-tested) | Adapter (firmware) |
|---|---|
| `gesture_engine` | `ui_touch_bridge` |
| `hid_core` | `hid_dispatch` + `usb_hid_device` |
| `config_store` record + store | `main/app_config.c` (NVS, writer task) |
| `net_status` FSM | `net_wifi.c` |
| `ui_app` (shell composition) | `main/ui_shell.c` (lock, clock, touch bridge, RELEASE_ALL) |

The core takes its platform through injected hooks: clock, RNG, storage, sinks and callbacks. The same core then runs in
host tests, in the firmware and in the simulator.

**Size and responsibility.**
- One file has one responsibility. Split it when it passes about 600 lines or starts doing two jobs.
- `main/` only wires components together. Logic that can be tested on its own belongs in a component.

**One owner per resource.**
- Only `touch_input` reads the GT911.
- Only the LVGL task calls `lv_*`, and the console or other tasks take the platform lock for short work only.
- HID goes only through typed intents → InputArbiter → HidScheduler, and every loss path ends in `RELEASE_ALL`.
- Only `net_status` drives the Wi-Fi driver.

**Shared files have one editor at a time.** `sdkconfig.defaults`, `idf_component.yml`, `dependencies.lock`, the root
`CMakeLists.txt`, `main/**` and the shared indexes (`docs/ADR/README.md`, `test/README.md`, `docs/ROADMAP.md`,
`THIRD_PARTY_NOTICES.md`) change through the integrator. Parallel work proposes those edits in its report.

**Parallel work** needs disjoint paths, reserved ADR numbers and separate build directories (`../build-host-<name>`,
`idf.py -B ../build-fw-<name> -D SDKCONFIG=...`). Nobody builds in another person's directory and nobody flashes
except the person who owns the board session.

## 4. Writing code that holds up

- **Check every API in the pinned headers** (ESP-IDF v6.1, LVGL 9.6, mbedTLS 4.1 in the workspace tooling). Never guess
  a name. LVGL 9.6 deprecates `lv_obj_add_flag`, `lv_obj_remove_flag` and `lv_obj_has_flag`: use
  `lv_obj_set_clickable`, `lv_obj_set_scrollable`, `lv_obj_set_hidden` and `lv_obj_is_hidden`. Build host tests with
  `-Werror` so deprecations show up.
- **Fail closed.** An unknown value reads "Unknown", never a guess. A refused action is counted, not retried silently.
- **List every DOWN → UP path** before you add a button, a key or a gesture: release, lost press, disable while held,
  delete while held, view change, HOST loss.
- **Test the error paths**, not only the happy path. Use sanitizers (ASan + UBSan) and seeded random or fuzz runs, and
  record the seed. Mutation checks (break the code on purpose and expect a failure) show that a test really guards
  something.
- **Lifecycle:** observers are bound to objects (`lv_subject_add_observer_obj`). Test create/delete cycles and expect a
  flat live-allocation count.
- **Generated or third-party code** keeps its own header and gets a line in `THIRD_PARTY_NOTICES.md`.
- Every authored source file starts with the three-line license header (`tools/check_license_headers.py`).
- In shell scripts, a heredoc can corrupt `\n` and `\\` inside C string literals. Write C sources with an editor or a
  script file, not with a heredoc.

## 5. Daily commands

```text
build      pwsh -NoProfile -File ../.tools/build.ps1 <name>        (log in ../evidence/build/<name>.log)
flash      pwsh -NoProfile -File ../.tools/flash.ps1 <name>        (COM3, one reset)
console    ../.tools/venv/Scripts/python -I test/hil/kvm_serial.py --timeout 20 cmd --name <tag> "heap" "tasks"
snapshot   ... cmd --name <tag> "ui snap 2"  then  python -I test/hil/ui_snapshot.py <log> <png>
state      "ui shell status"  "ui store"  "hid"  "usb"  "wifi"  "config"
host test  cmake -S test/<suite> -B ../build-host-<suite> -G Ninja -DCMAKE_C_COMPILER=clang && cmake --build ... && ctest ...
gates      python -I tools/verify_spec_manifest.py   (12/12)
           python -I tools/check_license_headers.py  (0 missing, 0 unlisted)
```
