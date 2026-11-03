# `test/remote_protocol/`

Host tests (L1/L2) of the CLIENT protocol v1 component, TASK-100. Pure C, no ESP-IDF, built with
AddressSanitizer and UndefinedBehaviorSanitizer.

```sh
export PATH=../.tools/llvm-mingw-20261020-ucrt-x86_64/bin:../.tools/espressif/tools/ninja/1.12.1:\
../.tools/espressif/tools/cmake/4.0.3/bin:$PATH
cmake -S test/remote_protocol -B ../build-host-remote -G Ninja -DCMAKE_C_COMPILER=clang
cmake --build ../build-host-remote
ctest --test-dir ../build-host-remote --output-on-failure
```

| Suite | Covers |
|---|---|
| `test_remote_json` | reader grammar, 1024 B / token / depth limits, duplicate keys, UTF-8 and escapes, number ranges, injection strings (AT-SEC-04 at the reader level) |
| `test_remote_validate` | one case per row of 03 §6.5 with the codes of §6.4/§7; AT-SEC-02, AT-SEC-03, AT-SEC-04, AT-SEC-07, AT-SEC-08, AT-EXT-19, SEC-103 scope |
| `test_remote_session` | 03 §6.3/§6.4/§6.6 messages, approval flow and 30 s timeout, lease renewal, session table, footprint; AT-CONN-04, AT-CONN-05, AT-CONN-06, AT-CONN-08 |
| `test_remote_vectors` | 03 §8 VEC-005, VEC-007, VEC-008, VEC-009; AT-CONN-07, AT-CONN-09, AT-FAULT-06; every release path and its reason |
| `fuzz_remote_frame` | seeded mutation fuzzer over the reader and the validator; AT-SEC-04, AT-EXT-17 |

`rp_harness.h` is the fake integrator: it records every intent, outbound frame, release reason and approval
call, and provides a frame writer in which single quotes stand for double quotes so the test literals stay
readable.

## Fuzzer

```sh
../build-host-remote/fuzz_remote_frame            # seed 20261102, 200000 inputs
../build-host-remote/fuzz_remote_frame <seed> <iterations>
```

Two invariants on every input: nothing crashes or trips a sanitizer, and a frame the validator did not accept
produces no HID intent at all. The accessors are also walked over every accepted document, so a token the
reader emitted can never make a reader read outside the frame. A failure prints the seed and the iteration
number, which together are the reproducer.

Recorded clean runs (200000 inputs each): seeds `20261102`, `1`, `2`, `777`, `20261103`, `4294967295`, plus
`31337` with 2000000 inputs.

## Hardware

Nothing here touches a board. The HOST-side behavior of the lease, of `RELEASE_ALL` and of the USB report
stream is `NOT_RUN_HW` until the HIL harness runs the same sequences against a real HOST.
