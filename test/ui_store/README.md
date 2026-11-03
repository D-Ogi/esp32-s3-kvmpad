# `test/ui_store` — TASK-060B host tests (real LVGL 9.6)

Builds the LVGL sources pinned by `dependencies.lock` (`managed_components/lvgl__lvgl`, fetched by `idf.py reconfigure`)
as a static library with the address sanitizer, plus `ui_core` with the strict flags of `test/host_unit`
(`-Wall -Wextra -Werror -Wconversion`, ASan + UBSan). LVGL runs headless on its test display (1024×600) with a custom
allocator (`host_lv_mem.c`) that counts live `lv_malloc()` blocks and bytes exactly.

```sh
cmake -S test/ui_store -B ../build-host-ui -G Ninja -DCMAKE_C_COMPILER=clang
cmake --build ../build-host-ui && ctest --test-dir ../build-host-ui --output-on-failure
```

| Suite | Covers |
|---|---|
| `test_ui_mailbox` | level semantics per source, late (stale) domain sequences incl. wrap, invalid enums, coalescing, event ring overflow → resync, lock pairing; 4 × 20 000 random publish/take/event operations converge to the model (AT-UIFW-03, store level) |
| `test_ui_actions` | 07 §3 value ranges, local vs domain routing, refusals never forwarded, request ids, 01 §6.1/§6.2 wording |
| `test_ui_store` | subjects UNKNOWN until published; observers bound through objects die with the widget and store deinit detaches the rest without use-after-free (AT-UIFW-09); dropped/late snapshots and lost events converge (AT-UIFW-03); 10 000 bind/delete cycles with zero live-allocation growth (AT-UIFW-02, store level); every test leaves the LVGL heap as it found it |

`lv_conf.h` mirrors the firmware options that matter for `ui_core`; the first text render creates one long-lived LVGL
renderer allocation, so the heap baseline is taken after a warm-up render.
