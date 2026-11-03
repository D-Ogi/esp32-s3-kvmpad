# `test/net_status` — TASK-080 host tests

Pure-C tests of the Wi-Fi link state machine (`components/net_status/src/net_fsm.c`) with a fake clock and seeded
jitter. Strict flags, ASan + UBSan.

```sh
cmake -S test/net_status -B ../build-host-net -G Ninja -DCMAKE_C_COMPILER=clang
cmake --build ../build-host-net && ctest --test-dir ../build-host-net --output-on-failure
```

| Test | Covers |
|---|---|
| not configured | no radio activity without a network (local-only, AT-CONN-03 at FSM level); saving one starts a full connect |
| full connect | associated is not "has an IP"; the cache is stored only after an IP |
| cached connect | ADR-017 fast path; an unchanged record is not written again; an invalid record is ignored |
| cached timeout / reject | after 3 s or a refusal: cache invalidated, full connect at once, our own abort echo ignored (AT-EXT-02 at FSM level) |
| backoff | exponential, capped, ≤ 25 % jitter, no attempt before the deadline (FR-406), reset after a success |
| drop / DHCP / stop / forget | a dropped link retries the cached AP; DHCP timeout; stop from every state ignores late driver events; forgetting the network while connected |
| no secret | the cache record (≤ 8 bytes) and the status have no string field (AT-EXT-03, structure) |
| random storm | 4 × 50 000 random driver events: `ip_present` only when connected, every transient state has a deadline, backoff within bounds, at most cached + full attempt without a backoff or a success between them |
