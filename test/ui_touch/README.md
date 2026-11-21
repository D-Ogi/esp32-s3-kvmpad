# `test/ui_touch` — TASK-060E host tests

Pure-C tests of `components/ui_touch_bridge/src/touch_router.c`, linked with the real `gesture_engine` and
`touch_input` sample helpers. Strict flags of `test/host_unit` (`-Wall -Wextra -Werror -Wconversion`, ASan + UBSan).

```sh
cmake -S test/ui_touch -B ../build-host-touch -G Ninja -DCMAKE_C_COMPILER=clang
cmake --build ../build-host-touch && ctest --test-dir ../build-host-touch --output-on-failure
```

| Test | Covers |
|---|---|
| Delayed display refresh | Queues 310 ms of simulated touch samples, then replays them through the gesture engine. Checks tap-and-drag, two-finger scrolling, cancellation after sensor silence and recovery after lift. |
| Direction changes during refresh | Compares the HID events from immediate and delayed reads for out-and-back pointer strokes, two-finger scrolling, opposing fingers, taps with jitter and a second tap that crosses the drag threshold by one pixel. |
| chrome tap / pad drag | target capture at touch-down; pointer follows only the chrome contact; pad samples keep `sample_seq` and timestamp |
| conflicts | pad while chrome is pressed, chrome while pad is held, second chrome contact → `BLOCKED` until lifted (AT-UIFW-04) |
| cancel / invalid | `ptr_cancel` (no click) and an empty pad sample; no target survives a cancel |
| slot reuse | a lifted contact's target never leaks into the next contact with the same slot |
| real gestures | the router feeding `kvm_gesture_feed()`: a two-finger tap gives exactly one right click; a rail press during a pad drag never presses the pointer |
| random invariants | 200 000 random samples: safety (no pointer press from a pad contact, never two owners) and liveness (a fresh contact on free ground is never blocked) |

The liveness invariant was checked by mutation: re-introducing the stale-target bug gives thousands of violations.

The local pointer test links the production bridge task to the simulator's real LVGL. It queues two complete taps before one pointer read on a rotated portrait display, checks the order of two button clicks, and exercises an ordinary slider, touchpad capture, cancellation and a popup over a key. Run against an existing Ninja simulator build:

```sh
python test/ui_touch/check_local_pointer.py --sim-build ../build-sim --out ../build-local-pointer
```
