# 06 — Architecture Decision Records (ADR), risks and issues to verify

**Version:** 1.1.0 · **Principle:** no arbitrary assumptions hidden in the code. Every decision has a status, variants and a confirming test.

## 1. Status labels

- **ACCEPTED (REFERENCE)** — the accepted recommendation for the current implementation; it can be changed by a new ADR, not by a silent refactor.
- **PENDING-HW** — the hardware/integrator must confirm it; it does not stop all the work, only the dependent tasks.
- **PENDING-PRODUCT** — a decision of the product owner before the marked milestone; if there is no answer, use the indicated safe default.
- **EXPERIMENT** — a comparative test, without declaring a final result.
- **REJECTED** — the solution is not allowed in v1.

## 2. ADR-001 — What exactly do KVM and CLIENT mean?

**Status:** ACCEPTED (REFERENCE), to be confirmed by the project owner before M7.  
**Decision:** DEVICE emulates a keyboard+mouse to HOST over native USB HID. CLIENT is a trusted application reachable over Wi-Fi that, once authorized, can control HOST through DEVICE. Statuses: HOST=USB state, CLIENT=authentic session.  
**Why:** this makes consistent use of the native USB, the GT911 and the 2.4 GHz Wi-Fi of this board, and at the same time gives a correct, secure division of roles.  
**What it is not:** a videograbber / HDMI capture / full video streaming. If "real KVM-over-IP with video" is planned, a separate capture device or an agent/host stream will be needed.  
**Alternatives:** (A) HID local only (a simple release), (B) remote CLI with a host agent (a different topology), (C) full HDMI-KVM (extra hardware).  
**Gate:** before TASK-100, confirm what CLIENT is to be (browser/native/a component of another system) and who provides the video, if it is needed.

## 3. ADR-002 — Encryption and the browser-based remote CLIENT

**Status:** PENDING-PRODUCT before TASK-100; **safe default: Remote OFF**.  
**Reference decision:** WSS over verified TLS and a per-client session with physical pairing/approval; no plaintext `ws://` in production.  
**To choose from:**

| Variant | Characteristics | Recommendation |
|---|---|---|
| A. Native CLIENT with certificate pinning | simple for a dedicated app; fingerprint distribution after an out-of-band pair | **preferred if we control the app** |
| B. Browser + gateway reverse proxy and a valid cert from a public/trusted CA | browser without a warning; a separate backend, the gateway→DEVICE segment must be secured | preferred if a browser is required |
| C. Browser + a local domain/private CA | requires a trusted CA on the users' devices | only for a managed fleet |
| D. `ws://`/HTTPS self-signed without trust/verification disabled | easy demo, real MITM risk | **REJECTED** |

**Gate:** a demonstration of working HTTPS/WSS on a real client and device with correct cert validation. "TLS enabled" in the code alone is not enough. A full threat model, AT-SEC-01..08.

## 4. ADR-003 — ESP-IDF/components, stability and versioning

**Status:** ACCEPTED (REFERENCE) + PENDING-HW proof.  
**Base stack / updated after ADR-013 (2026-11-01):** IDF 6.1 stable, LVGL 9.6.0, `esp_lvgl_adapter` 0.7.4 as the proof-of-compatibility candidate (or a pinned `esp_lvgl_port` 2.9.0 as a controlled fallback), esp_lcd_touch_gt911 1.2.1, esp_tinyusb 3.0.0.  
**Principle:** a version does not become ACCEPTED when it is downloaded; only after a build, link and smoke test on the board.  
**Risk:** breaking TinyUSB 3.x API, IDF 6.x I²C/USB changes and a conflict between the manufacturer's example (on an older IDF) and the current BSP.  
**Plan:** TASK-000 creates the lockfile; TASK-010 separates the BSP; a fallback to the supported IDF 5.5.x branch only after an ADR with facts, not "because it worked for me in Arduino".

## 5. ADR-004 — Power supply, VBUS separation and HOST offline visibility

**Status:** PENDING-HW before an independent status while HOST is disconnected is required.  
**Fact:** if the USB HOST PORT is the only source of 5V, powering HOST off may power the ESP and the LCD off; the system will **not** display the disconnection then.  
**Default decision:** in v1 the device may use HOST power, in which case the `HOST DISCONNECTED` status applies only as long as DEVICE is powered.  
**If permanently active control is required:** an approved alternative power supply schematic for DEVICE with separation of the USB HOST port and no reverse VBUS feed. A 1S PH2.0 battery or a dedicated power path is possible — it requires an analysis of the Waveshare schematic and a test on the PCB.  
**Forbidden:** plugging in two unverified power supplies at the same time, injecting 5V into USB HOST and guessing the connections.

## 6. ADR-005 — USB device: Boot interfaces and report compatibility

**Status:** ACCEPTED (REFERENCE), HIL required.  
**Decision:** a separate boot-compatible keyboard and relative mouse; USB HID device topology.  
**Alternative:** one HID composite with report IDs (may lower UEFI compatibility), only HID consumer controls; they require a proof of compatibility.  
**Gate:** `lsusb -v`, Windows descriptor enumeration, UEFI keyboard arrows/Enter/Esc and mouse; host Caps LED output.  
**Legal note:** the VID/PID must not be used unlawfully in production; a test VID/PID only in the prototype.

## 7. ADR-006 — Native LVGL instead of embedded HTML

**Status:** ACCEPTED.  
**Decision:** HTML/CSS/JS = an interactive design pattern and a layout test, firmware = LVGL9 native ESP-IDF.  
**Why:** there is no full browser on the ESP32-S3; better memory, GPU-free RGB control, lower latency and deterministic events.  
**Prohibition:** WebView/Chromium/JS interpreter as an unjustified UI runtime.  
**Gate:** a real 800×480 framebuffer, hitboxes ≥44px, input latency under load.

## 8. ADR-007 — A dedicated GestureFSM vs LVGL9 gesture detection

**Status:** ACCEPTED (REFERENCE), an experiment with the LVGL9.4 gesture API only as an auxiliary.  
**Decision:** our own pure gesture FSM reads the GT911 track IDs and emits HID intents. LVGL takes input for the UI hit-test, but is not a separate source of HID gestures.  
**Why:** testability, full control of 1/2/3/5 finger conflicts, restart/timeout, no accidental clicks.  
**Risk:** duplication of the gesture framework; we compensate by extracting a single module and with comparative tests.

## 9. ADR-008 — LCD RGB single/double PSRAM framebuffers

**Status:** EXPERIMENT in TASK-020.  
**Options:** A `fb PSRAM + 2×10 lines bounce`, B `+2×20 lines bounce`, C `double buffer` with acceptable memory use.  
**Why:** Espressif describes a conflict of PSRAM bandwidth with Wi-Fi/flash and the dependence of bounce on the cache; Waveshare, in the v5.3 demo, notes a benchmark of ~26 FPS single-core and a panel refresh of ~41 FPS at PCLK 21MHz (not a measurement of our firmware).  
**Final decision:** after the p95 touch-to-HID, screen shift, RAM margin and 72h soak tests; readability + HID quality are more important than a high FPS.  
**Forbidden:** forcing 60 FPS and 120 MHz PSRAM without a measurement and without a flash/OTA test.

## 10. ADR-009 — Flash 8 MiB or 16 MiB?

**Status:** PENDING-HW, blocks the design of the OTA partition table.  
**Reason:** in part of the documentation Waveshare states 16 MB Flash / 8MB PSRAM and in a table 8 MB Flash; the SKU/revision may differ.  
**Decision:** `esptool flash_id`, the module number, an `esp_flash_get_size` log, PSRAM detection → a hardware record in the repository in `hardware/boards.md`.  
**After confirmation:** generate the partitions with ≥2 OTA slots and enough space for a signed app/rollback, do not guess.

## 11. ADR-010 — HOST state: the truth at the USB level, not that of a computer application

**Status:** ACCEPTED.  
**Semantics of `Active`:** TinyUSB configured/mounted, not suspended. We do not claim that HOST has a working window or that it played back a key; there is no such feedback in standard USB HID.  
**Possible extension:** an additional host-side agent heartbeat and a separate `Host Agent Online` field. It does not override the USB status.  
**Gate:** shutting down the Host App does not cause a false `USB_DISCONNECTED`; unplugging the cable causes a USB-unmount only when DEVICE still has power.

## 12. ADR-011 — LOCAL/REMOTE input arbitration

**Status:** ACCEPTED.  
**Decision:** physical LOCAL can always take over control; it triggers revoke + release + a new epoch. REMOTE must ask for a lease again with physical approval.  
**Why:** local safety and keeping control intuitive without races.  
**Risk:** an accidental local touch during a remote session interrupts the work; the solution: only a recognized HID action, and not an innocent interaction in Settings, initiates an override. During the pairing process LOCAL can always cancel the pairing.

## 13. ADR-012 — Security scope of the production firmware

**Status:** ACCEPTED (REFERENCE) subject to approval of the release procedures.  
**Decision:** Secure Boot v2 + Flash Encryption Release Mode + signed OTA + rollback + NVS security and a per-device token.  
**Risk:** irreversible fuses and loss of ROM USB OTG flash/recovery with certain security settings.  
**Condition:** the implementer may prepare the procedure and the commands, but may **not** perform `efuse burn` operations on the board without the operator's explicit approval and a confirmed recovery/key escrow plan.  
**Evidence:** secure boot verification and rejection of unsigned firmware, without damaging the device.

## 14. Risks (RACI / severity)

| ID | Risk | Weight | Probability | Prevention / verification | Owner |
|---|---|---|---|---|---|
| R-01 | USB/CAN shared GPIO19/20, wrong EXIO5 | BLOCKER | medium | CH422G LOW → USB, TASK-010 HIL | BSP |
| R-02 | Wrong GPIO number/a different PCB revision | BLOCKER | medium | schematic + sample + LED/color HIL | HW/BSP |
| R-03 | KVM interpreted as HDMI capture without the hardware | HIGH | high if not named | ADR-001, a clear boundary in the UI | Product |
| R-04 | Phantom/held Ctrl/drag on Wi-Fi loss | BLOCKER | medium | release_all, epoch, AT-HID-09 | HID |
| R-05 | Tap→two-finger scroll clicks accidentally | HIGH | medium | FSM, track IDs, vectors | Touch |
| R-06 | RGB shift/PSRAM bottleneck with Wi-Fi/flash | HIGH | medium | bounce A/B/HIL, OTA maintenance | LCD |
| R-07 | Flaky browser self-signed cert / plaintext WS | BLOCKER | high if a browser without the infrastructure | ADR-002, cert trust tests | Security |
| R-08 | Undetermined 8/16 MiB flash, incorrect partitions | BLOCKER | medium | `flash_id`, app image sizes, TASK-000 | Firmware |
| R-09 | VID/PID not authorized for the product | HIGH | medium | compliance pre-release | Product |
| R-10 | When HOST is powered off, DEVICE powers off (power from HOST) | MEDIUM/HIGH | high without AUX | ADR-004, power schematic | HW |
| R-11 | Remote HID signals without auth/local approval | BLOCKER | medium | AT-SEC-01..08 | Security |
| R-12 | Enabling an eFuse without recovery, brick | BLOCKER | medium | manual approval + recovery test | Release |
| R-13 | No compatibility of TinyUSB 3.0/IDF6.1 with the vendor sample | HIGH | medium | build proof M0 + pin dependencies | Build |
| R-14 | UI overdesign lowers the FPS and delays HID | MEDIUM | medium | native LVGL + NFR | UX/Firmware |
| R-15 | An API name or HIL results are invented instead of verified | HIGH | high | AGENTS.md, reviewer, output logs | Coordinator |
| R-16 | No real compatibility of boot HID with UEFI | MEDIUM | medium | AT-HID-01 + real BIOS/UEFI | HID |

## 15. Product questions — they do NOT block starting local HID

| Question | Safe default until an answer is given | When the decision must be made |
|---|---|---|
| Is CLIENT a browser, a desktop application or a gateway? | Remote OFF, local USB HID fully functional | before M7 |
| Where will the video in a full KVM come from? | No video in v1 | before any "Remote Screen" feature |
| Must the HOST OFF status be visible after its power has been cut? | No, when there is no AUX power | before designing the power supply of the enclosure |
| Do we distribute the devices commercially? | prototype/development VID/PID, production requires its own lawful USB identity | before the production release |
| Is macOS required? | basic HID without certifying macOS shortcuts | before declaring macOS support |
| Is a horizontal scroll needed in BIOS? | a vertical scroll in the OS; the horizontal fallback is described | before finalizing the enhanced descriptor |
| Must the keyboard produce Polish characters regardless of the HOST layout? | NO; USB HID usage + HOST layout | before designing IME/text injection |
| Is it to work on battery? | Not required, unless a separate power path is provided | before the enclosure/power design |

## 16. Procedure for proposing a new decision

Create in `docs/ADR/ADR-<num>-<slug>.md`:

```markdown
# ADR-0xx — Title
Status: PROPOSED | ACCEPTED | REJECTED
Date: YYYY-MM-DD
Context: observable facts with source links
Options: A/B/C and pros/cons
Decision: exact selected option
Consequences: security/memory/API/UX
Tests: AT-... and evidence paths
Migration: steps if replacing existing implementation
Approver: project owner / integration lead
```

**MUST:** nobody changes ADR-001/002/004/012 on their own without approval, because they concern the topology, security, the electrical design and irreversible hardware settings respectively.


## ADR-013 — KVMpad UI Core on LVGL 9.6

**Status: ACCEPTED (REFERENCE), PENDING-HW stack proof.** The decision is described normatively in `07-KVMpad-UI-FRAMEWORK-ARCHITECTURE.md`. No own DOM/DSL/Redux; we use Observer, LVGL widgets/styles, our own typed actions/router + a single GT911 touch frame router. The default candidate is `esp_lvgl_adapter` 0.7.4; it requires HIL before replacing the adapter existing in the integration. EEZ Studio only as an optional no-Flow generator (TASK-061). LVGL Pro runtime XML is not a dependency.
