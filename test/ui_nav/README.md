# `test/ui_nav` — TASK-060C host tests (real LVGL 9.6)

Router and overlay stack of `components/ui_core` (`ui_router.c`, `ui_overlay.c`) against the LVGL sources pinned by
`dependencies.lock`, on a headless 1024×600 test display with the LVGL test pointer (real hit testing, press, release,
click). Shares `lv_conf.h` and the counting allocator with `test/ui_store`. Strict flags, ASan + UBSan.

```sh
cmake -S test/ui_nav -B ../build-host-nav -G Ninja -DCMAKE_C_COMPILER=clang
cmake --build ../build-host-nav && ctest --test-dir ../build-host-nav --output-on-failure
```

| Test | Covers |
|---|---|
| `test_init_validation` | invalid layouts, duplicate / settings-as-view / zero-id descriptors refused, nothing left behind |
| `test_atomic_switch_order` | create (lazy) → cancel + HID release request → `on_leave` → `on_enter` → `active_view` published → pad re-read (07 §5); same-view show is a no-op without a cancel |
| `test_nav_with_key_down_and_drag` | a held key or drag is released before the old view leaves; a refused release request is counted and the switch still completes (the HID core stays the safety owner) — AT-UIFW-08 at router level |
| `test_lazy_cache_and_failures` | each view created once (cache ≤ 3); a failed create leaves the old view untouched with nothing cancelled; re-entrant show/overlay from `on_enter` refused |
| `test_blocking_overlay_absorbs_clicks` | a blocking modal cancels the drag, takes the pad away, absorbs clicks on the view below and blocks navigation; after dismiss the pad and the view work again |
| `test_overlay_stack_rules` | non-blocking panel keeps the pad; duplicates, stack overflow (max 2), unknown ids and failed creates refused; LIFO dismiss; dismiss-all |
| `test_dismiss_from_click_1000` | the close button dismisses its own overlay (deferred delete) 1000 times; the view below still receives every click; zero live-allocation growth (AT-UIFW-10) |
| `test_10000_switches_no_growth` | 10 000 view switches with status updates: zero live-allocation growth, three views created (AT-UIFW-02) |
| `test_deinit_orders` | router deinit closes overlays, leaves and destroys views; store deleted before the router while views still observe its subjects: no use-after-free (AT-UIFW-09) |

Mutation check: moving the cancel step after `on_leave` makes the suite fail (2 failures).
