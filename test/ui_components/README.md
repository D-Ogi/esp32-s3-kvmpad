# `test/ui_components` — host tests for `ui_theme` and `ui_components`

These suites compile the real LVGL 9.6 sources from `managed_components/lvgl__lvgl` together with
`components/ui_theme` and `components/ui_components`, and run them against a headless 1024x600
RGB565 display (ADR-026 TECH-087). No ESP-IDF, no board and no serial port are involved, so they
run on every change; hardware claims stay `NOT_RUN_HW` until the panel is available.

The allocator is `LV_STDLIB_CUSTOM` (`host_lv_mem.c`), so every test can assert an exact live
allocation and byte count. AddressSanitizer and UndefinedBehaviorSanitizer are on for the project
sources, ASan for LVGL.

## Build and run

Tools live in the workspace directory above the repository, never on `C:` and never globally.

```sh
export PATH=../.tools/llvm-mingw-20261020-ucrt-x86_64/bin:../.tools/espressif/tools/ninja/1.12.1:../.tools/espressif/tools/cmake/4.0.3/bin:$PATH
cmake -S test/ui_components -B ../build-host-uicomp -G Ninja \
      -DCMAKE_C_COMPILER=clang \
      -DKVM_SNAPSHOT_DIR=O:/Praca/KVMpad/evidence/build/ui_components
cmake --build ../build-host-uicomp
ctest --test-dir ../build-host-uicomp --output-on-failure
```

`KVM_SNAPSHOT_DIR` is optional and must point **outside** the repository; leaving it empty
disables snapshot writing. The directory is created if it does not exist.

## Suites

| Target | What it covers |
|---|---|
| `test_ui_theme` | token floors (spacing, touch targets, 1024x600 geometry), the topbar width budget, the four Montserrat roles, severity color **and** glyph (FR-005), init/deinit idempotence, styles surviving a second init, every button and key variant |
| `test_ui_components` | status item projection and clipping, topbar slots and the FR-712 keep-awake indicator, action-key DOWN/UP pairing including press-lost, disable and delete, segmented selection and router ownership, 10 000 shell build/teardown cycles at a constant live allocation count |
| `test_ui_overlay` | modal scrim blocking, dismiss paths, idempotent close, 1 000 modal cycles, toast timeout and self-deletion, 2 000 toast cycles |
| `test_ui_snapshot` | a full shell in three states, hit-box geometry (size, bounds, overlap) and the PPM snapshots |

## Snapshots

`test_ui_snapshot` writes binary PPM files named `shell_unknown`, `shell_active`, `shell_host_lost`
and `shell_modal`. They are raw render output for a human to look at; nothing in the suite compares
them to a golden image, so a visual change never fails the build on its own.

Snapshots and any other raw artifact belong in `../evidence/`, outside the repository
(`CONTRIBUTING.md`). They contain no keystrokes, tokens or pairing secrets: every string in the
fixtures is a literal in the test sources.
