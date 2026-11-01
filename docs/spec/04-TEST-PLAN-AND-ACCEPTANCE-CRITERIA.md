# 04 — Test plan, HIL and acceptance criteria

**Version:** 1.0.0 · **Status:** mandatory test plan · **Definition of completion:** repeatable tests, a PASS/FAIL result, logs and environment, not just "it looks good".

## 1. Test layers

| Layer | Where | What it measures | Evidence artifact |
|---|---|---|---|
| L0 — static | CI, host | types, format, compilation, static analysis, lockfile | CI log, warnings count |
| L1 — unit | native C test runner | deterministic GestureFSM, HID mapper, ownership, payload validator, NVS migrate | JUnit + seed + coverage |
| L2 — fuzz/property | host CI | any legal/illegal touch/packet sequences | crash reproducer + corpus |
| L3 — device integration | real ESP32 | GT911, CH422G, RGB, PSRAM, USB, Wi-Fi without HOST | serial log, firmware hash |
| L4 — HIL USB | real ESP32 + HOST | physical enumeration, actual HID key/button report, reconnect | usbmon/hid trace, device log, photo of the setup |
| L5 — UX | panel of people | hitting the keys, ergonomics and gestures | test protocol and numeric result |
| L6 — long-running + security | HIL + LAN | 72h soak, power cycling, TLS, ownership, invalid packets | metrics CSV, security report |

**Implementers' obligation:** L0/L1 without hardware may be marked `PASS_SW`; L3–L6 without a device are always `NOT_RUN_HW`. A "PASS" based solely on a simulation or on the implementer's declaration is not allowed.

## 2. Lab equipment

- Waveshare ESP32-S3-Touch-LCD-7 with the PCB revision and the Flash/PSRAM size noted down.
- Two **distinguished** USB cables: native OTG USB-C→HOST and USB-to-UART USB-C→debug station, if the power/recovery test allows it; do not assume safe simultaneous powering without the schematic.
- HOST A: a Windows 11 PC; HOST B: a Linux PC (`hid-recorder`, `evtest`, `usbmon`, Wireshark USBPCap on Windows are also preferred). A manual BIOS/UEFI test on at least 1 compatible PC.
- A 2.4 GHz Wi-Fi AP with the ability to restart/disconnect, a network load generator, a trusted CLIENT (native or with a working TLS certificate/gateway).
- An external supply for examining HOST disconnected only after checking backfeed/USB power isolation on the schematic.
- Optionally: a high-speed camera/contact measurement/photodiode; for p95 touch-to-HID, internal monotonic timestamps + a confirmed trace of the reports are sufficient.
- Lab log `hardware-revision`, `esp-idf-version`, `dependencies.lock hash`, `firmware git SHA`, `power scheme`, `HOST OS build`, the date and time of the test.

## 3. Unit test protocol: gestures

| ID | Action / vector | PASS result |
|---|---|---|
| AT-GEST-01 | VEC-001 from `03`, a 1-finger tap | exactly 1 LEFT_DOWN + LEFT_UP, no scroll/move |
| AT-GEST-02 | one finger moves 50 px and up | the sum of the deltas is correct, no click |
| AT-GEST-03 | long press 550 ms with jitter <10 px | exactly 1 RIGHT_CLICK, not LEFT |
| AT-GEST-04 | long press with >10px of movement | there is no RIGHT_CLICK |
| AT-GEST-05 | VEC-002 two-finger scroll | wheel, no L/R click and no pointer move |
| AT-GEST-06 | a short parallel 2-finger tap | 1 RIGHT_CLICK; not 2 × LEFT |
| AT-GEST-07 | VEC-003 points in reverse order | no jump of the centroid or of the cursor |
| AT-GEST-08 | VEC-004 2→1→0 fingers | no click on the release of the last finger |
| AT-GEST-09 | 3/4/5 fingers in any order | cancellation; no HID input beyond the required release |
| AT-GEST-10 | double-tap-and-drag | exactly 1 `LEFT_DOWN` of the second contact; `LEFT_UP` after it ends |
| AT-GEST-11 | 120ms no data / I²C error | cancel + release, error counter |
| AT-GEST-12 | Drag lock, mode switch, timeout | after every path a final `LEFT_UP`, ledger zero |

**Property-based (PBT) invariants:** for a random sequence of length 1..10000: the `downs-ups` sum is non-negative during operation; after `CANCEL/RELEASE_ALL` all bitmaps are zero; events with an unknown trackId do not create a delta >the limit; 2-finger scroll does not produce a single LEFT click; no `3+touch` generates new keys and buttons. Run ≥100000 random sequences per nightly (record the failing seed).

## 4. Keyboard and HID report tests

| ID | Action | PASS result |
|---|---|---|
| AT-KEY-01 | Every QWERTY character, digits, punctuation, the characters `\ []` | 100% map to the correct USB usage, no ASCII→HID confusion |
| AT-KEY-02 | Esc, Tab, Enter, Backspace, Delete, Insert, Home/End, PgUp/PgDn | correct usage + UP, no Delete/Backspace mix-up |
| AT-KEY-03 | F1–F12 and layer switching | 12 codes, Fn only a local layer |
| AT-KEY-04 | Shift/Ctrl/Alt/AltGr/Win | 100% correct modifier bits in HID |
| AT-KEY-05 | Tap Ctrl→C | Ctrl+C 1x, the next ordinary key without Ctrl |
| AT-KEY-06 | double-tap Shift→LOCKED; tap unlock | the physical state and the UI agree, no stuck |
| AT-KEY-07 | CapsLock LED output HOST | the UI shows the actual LED or Unknown, it does not guess |
| AT-KEY-08 | quick tap of a key 1000x | 1000 DOWN + 1000 UP; no overflow/missed |
| AT-KEY-09 | Wi-Fi disconnect during KEY_DOWN | LOCAL HID still works; remote release if there is a remote owner |
| AT-KEY-10 | Typing after USB reenumeration | a neutral start, without old modifier bits |
| AT-HID-01 | USB descriptors | the host sees the required HID interfaces; `bInterfaceSubClass/Protocol` consistent with Boot |
| AT-HID-02 | HID report format | exact lengths, keyboard 8B boot, mouse consistent with the descriptor |
| AT-HID-03 | complex 6KRO states | reports do not exceed 6 non-modifier usages, gracefully reject overflow |
| AT-HID-04 | split dx/dy | the sum of the signed deltas is consistent; no overflow in the int8 conversion |
| AT-HID-05 | Ctrl+Alt+Del chord | the correct 4 phases and a neutral report |
| AT-HID-06 | Alt+Tab, Ctrl+Shift+Esc, Win+D | correct orders, zero stuck |
| AT-HID-07 | Alt+F4, Print Screen | a host-dependent effect is not treated as confirmed executed if there is only a USB submit |
| AT-HID-08 | chord interrupted by USB suspend/revoke | ledger zero, a neutral report after mount |
| AT-HID-09 | 10k shortcuts with a random disconnect | 0 stuck modifiers, 0 phantom repeats |

## 5. UI tests 800×480

| ID | Validation | PASS |
|---|---|---|
| AT-UI-01 | LVGL screenshot 800×480 for the 3 modes | no out-of-bounds/scroll; the modes do not overlap |
| AT-UI-02 | hitboxes of the whole UI | all ≥44×44px; especially the corners and the bottom row |
| AT-UI-03 | mode change | 1 tap, without keyboard/touch input resulting from event bubbling |
| AT-UI-04 | host/client status | independent icons and text; OFFLINE detection in line with the source-of-truth |
| AT-UI-05 | settings and on-screen overlay | no key is covered in the active mode, control of invalid settings |
| AT-UI-06 | render under stress/flash | no permanent RGB drift, no GUI freeze and no random HID |
| AT-UI-07 | macOS style reference without closed-license fonts | a legal licensed font, contrast and legibility |
| AT-UI-08 | the host's image on the DEVICE | there is no fake video capture, the cursor does not "pretend" to be the HOST cursor |

**Visual regression test:** evaluate the image from the real LVGL framebuffer comparatively against `reference/` with a fixed theme/resolution. RGB565 color and font antialiasing differences are acceptable; the comparison cannot replace an interactive touch test. Do not declare pixel-by-pixel conformance with the HTML rendering.

## 6. Connections and ownership

| ID | Action | PASS result |
|---|---|---|
| AT-CONN-01 | USB unplug while external-powered | HOST=Disconnected, CLIENT still consistent with the truth |
| AT-CONN-02 | turn off the HOST when powered from it only | the DEVICE may lose power — the documentation does not declare the ability to display offline |
| AT-CONN-03 | USB mounted, no Wi-Fi | HOST Active, CLIENT Offline/Off, local HID works |
| AT-CONN-04 | Wi-Fi connected without an auth session | CLIENT Available/Disconnected, never `Connected` |
| AT-CONN-05 | auth session without a control lease | CLIENT Connected, remote input rejected |
| AT-CONN-06 | grant remote with physical approval | remote input works, the lease epoch applies |
| AT-CONN-07 | local takeover during a REMOTE drag | remote LEFT_UP, local works, old seq rejected |
| AT-CONN-08 | heartbeat lost for 3s | CLIENT stale + revoke; pressed-ledger zero |
| AT-CONN-09 | VEC-008 USB umount→mount | neutral state; zero old keydown |
| AT-CONN-10 | 100 USB disconnects/re-enumerations | no crash and no old HID event replays |

## 7. Fault injection / recovery

| ID | Fault | Required effect |
|---|---|---|
| AT-FAULT-01 | 100 GT911 I²C errors | no phantom click, the UI indicates the problem, recover/backoff |
| AT-FAULT-02 | NVS read invalid checksum | factory-safe defaults, no crash; LOCAL USB works |
| AT-FAULT-03 | NVS written under RGB stress | the GUI may have a transient frame drop; no permanent shift, HID does not lose a keyup |
| AT-FAULT-04 | not enough heap | handle it explicitly, without accidental HID |
| AT-FAULT-05 | USB endpoint not-ready | retry only at phase granularity, in a safe way; shortcut never duplicates |
| AT-FAULT-06 | session close in mid-keyboard chord | release all; no resumed old input |
| AT-FAULT-07 | OTA while drag lock | maintenance + release before flash |
| AT-FAULT-08 | power cut during OTA | the previous slot is bootable; no NVS corruption |
| AT-FAULT-09 | watchdog reset | neutral HID after boot; count the reset reason |
| AT-FAULT-10 | factory reset / invalid pairing state | tokens and roles removed, no open unauthorized network |

## 8. Security tests (SEC)

| ID | Negative case / audit | PASS |
|---|---|---|
| AT-SEC-01 | a `ws://` connection, TLS invalid cert | blocked in release, no downgrade |
| AT-SEC-02 | unauthorized input / no control lease | 0 HID reports |
| AT-SEC-03 | replay `seq`, stale lease epoch | rejection + metric, 0 HID |
| AT-SEC-04 | SQL/script strings/unknown types/overlong JSON (>1024 B) | parser fail closed, no crash |
| AT-SEC-05 | malicious `Origin` browser | WS handshake rejected |
| AT-SEC-06 | brute force pair / expired QR | rate-limited, attempt cap, no session |
| AT-SEC-07 | remote endpoint attempts OTA/reset/exec | no such API or permission deny |
| AT-SEC-08 | credential recovery via UART logs/status | no token/password/typed keys in the logs |
| AT-SEC-09 | flash encryption/secure boot path release | signature and recovery procedure, eFuse after approval |
| AT-SEC-10 | heap/race sanitizer (host) + protocol fuzz | zero UAF/OOB/integer overflow, a report from the scanner |

**Security release gate:** critical authorization / input injection problems block the release even when the GUI works. The WSS test must not be replaced with a "over `ws://` for a moment" test.

## 9. Performance tests (NFR)

### 9.1 Measurement methodology

- At least 10 000 latency samples in the USB-mounted state and 10 000 under network + RGB stress; report p50/p95/p99 and a histogram; tests at 240 MHz and with the recorded memory configuration.
- The interval `touch_sample ISR/task timestamp → hid_report_queued/completed timestamp` measured monotonically inside the DEVICE. A separate `USB callback report completed → HOST event observed` requires correct synchronization or USB trace analysis; do not add up times from unsynchronized clocks.
- The LVGL FPS may differ from the actual scanout PCLK; report two separate metrics if available.
- Enable Wi-Fi STA traffic, remote ping, pointer move and NVS operations; do not optimize only in an empty GUI.

| ID | Parameter | PASS/target |
|---|---|---|
| AT-PERF-01 | sample→HID enqueue median/p95 | ≤20 ms / ≤45 ms |
| AT-PERF-02 | UI FPS while active | ≥25 frame render FPS; target 30, no freeze |
| AT-PERF-03 | GUI navigation latency | p95 ≤100 ms |
| AT-PERF-04 | touch I²C throughput 1/2/5 pts | no loss of ID consistency, a real Hz measurement |
| AT-PERF-05 | min internal free heap | ≥64 KiB soft floor or an ADR with evidence of low risk |
| AT-PERF-06 | 72 h soak | 0 crash, 0 phantom input, no monotonic heap leak |
| AT-PERF-07 | Wi-Fi stress + USB keyboard typing | 0 stuck/phantom keys, latencies within NFR |
| AT-PERF-08 | startup UI on board | ≤3.5s target and a report, even if it is not met — issue/ADR |
| AT-PERF-09 | OTA flash + RGB | no permanent scanout drift; input blocked until the health-check |

**A failed performance target:** do not hide it; ticket, profiling, a comparison of the `single_fb+10-lines` vs `20-lines` vs `double_fb` variants. Do not guess that `PSRAM=120MHz` is always better.

## 10. Practical UX — real hands

**Minimum set:** 3 people with different finger sizes, 10 attempts of each activity: tap, right tap, two-scroll, drag, Cmd/Shortcut, Ctrl+C, F12, Delete, mode switching. Measure the hit error and the time, note down accidental gestures. The position of the device on the desk, dry/wet fingers, the brightness level.

**Initial threshold:** ≥95% successes for the basic touch targets in the trial tasks; 0 accidental Ctrl+Alt+Del and "stuck Ctrl". Report separately when the problem stems from the gesture threshold, the key geometry, the orientation or an actual USB failure.

## 11. Release test matrix

| Profile | L0/L1 | L3 ESP32 | L4 Windows | L4 Linux | L5 UX | L6 Security/Soak |
|---|---|---|---|---|---|---|
| docs-only PR | doc lint | n/a | n/a | n/a | n/a | n/a |
| PR internal gesture | MUST | SHOULD | optional | SHOULD | spotcheck | fuzz MUST |
| PR USB/descriptors | MUST | MUST | MUST | MUST | PRE-OS manual | WSS n/a |
| PR network/auth | MUST | MUST | local HID regression | test | consent UX | MUST |
| PR GUI/keymap | MUST | MUST | HIL key regression | HIL key regression | MUST | security smoke |
| RC release | MUST | MUST | MUST | MUST | MUST | MUST |

## 12. Definition of evidence and of the test report

Every report covers:

```yaml
test_id: AT-HID-05
result: PASS | FAIL | NOT_RUN_HW | BLOCKED
build_sha: "<git commit>"
firmware_sha256: "<sha256>"
hardware: "Waveshare ESP32-S3-Touch-LCD-7 rev <...>"
flash_psram: "<actual>"
usb_host_os: "<OS/build>"
idf: "<tag>"
components_lock_sha256: "<sha256>"
when_utc: "<ISO8601>"
steps: "<commands/touch inputs>"
observed: "<actual report/metrics>"
evidence: ["logs/xxx.txt", "usb/xxx.pcapng"]
issues: []
```

**MUST:** evidence from an emulator is not evidence of a test on a physical device; hard real-time and USB driver requirements must have a physical test. Image/pcap attachments must be sanitized (they must not contain typed passwords or tokens).

## 13. Release checklist — product sign-off

- [ ] ALL `AT-UI`, `AT-GEST`, `AT-KEY`, `AT-HID`, `AT-CONN`, `AT-SEC`, `AT-FAULT` from the required matrices: `PASS`, without `NOT_RUN_HW` in the critical paths.
- [ ] `NFR-001` latency and the 72h soak with measurable artifacts.
- [ ] Real USB on Windows/Linux + UEFI manual with results.
- [ ] The `RELEASE_ALL` requirement, 10k iterations without a stuck key/button.
- [ ] Operation without Wi-Fi + independent HOST/CLIENT status.
- [ ] Secure pairing + TLS/certificate plan + local approval.
- [ ] The signed/rollback update tested; eFuse programming approved separately.
- [ ] No credentials in the logs, no borrowed VID/PID in the product.
- [ ] Dependencies pinned, the code split into components, README build/recovery updated.
- [ ] Every deviation from the specification has an accepted ADR decision and a test.
