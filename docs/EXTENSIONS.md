# Requirement extensions after specification v1.1.0

The specification in `docs/spec/` is frozen. New requirements have IDs from ranges the spec does not use,
and the full text and rationale are in the ADRs. This file is the traceability index.

**ID ranges:** `FR-311`, `FR-7xx` (extension features), `TECH-07x`/`TECH-08x`, `SEC-10x`, tests `AT-EXT-xx`.

| ID | Level | Requirement (summary) | ADR | Task | Component | Tests |
|---|---|---|---|---|---|---|
| TECH-070 | SHOULD | `WIFI_PS_NONE` after measurement | 017 | TASK-080 | Wi-Fi (ADR-015) | AT-EXT-01 |
| TECH-071 | SHOULD | BSSID + channel cache, fallback to scan | 017 | TASK-080/090 | Wi-Fi, `config_store` | AT-EXT-02/03 |
| TECH-072 | SHOULD | 3rd HID interface: Consumer Control | 018 | TASK-050 | `usb_hid_device` | AT-EXT-04 |
| TECH-073 | SHOULD | `AC Pan` in the mouse report protocol | 018 | TASK-050 | `usb_hid_device`, `hid_core` | AT-EXT-06 |
| TECH-074 | MUST | Consumer in the ledger, DOWN→UP, `RELEASE_ALL` | 018 | TASK-050 | `hid_core` | AT-EXT-05 |
| TECH-075 | MUST | Consumer usage allowlist; remote only `command_id` | 018 | TASK-050/100 | `hid_core`, `remote_protocol` | AT-EXT-05 |
| FR-311 | SHOULD | "Media" group in SHORTCUTS | 018 | TASK-070C | `ui_views` | AT-EXT-05 |
| FR-701 | MUST | Type text only in CLIENT via `keyboard.key` | 019 | TASK-105 | CLIENT | AT-EXT-10 |
| FR-702 | MUST | character→usage tables per layout (US, PL-Programmer) | 019 | TASK-105 | CLIENT | AT-EXT-10/11 |
| FR-703 | MUST | interruption: Esc / network / local takeover | 019 | TASK-105 | CLIENT, `hid_core` | AT-EXT-12 |
| FR-704 | MUST | rate within the protocol limits, length limit | 019 | TASK-105 | CLIENT | AT-EXT-12 |
| FR-705 | MUST | CLIENT does not log the text | 019 | TASK-105 | CLIENT | review |
| FR-711 | SHOULD | Keep awake switch, OFF by default | 020 | TASK-070C/090 | `ui_views`, `config_store` | AT-EXT-07 |
| FR-712 | MUST | permanent indicator in the topbar | 020 | TASK-070C | `ui_components` | AT-EXT-07 |
| FR-713 | MUST | send conditions (MOUNTED, empty ledger, idleness) | 020 | TASK-050 | `hid_core` | AT-EXT-08/09 |
| FR-714 | MUST | 0 px report; ±1 variant only via an ADR | 020 | TASK-050 | `hid_core` | AT-EXT-07 |
| TECH-076 | MUST | `KVM_HID_KEEPALIVE`, origin SYSTEM, via the arbiter | 020 | TASK-050 | `hid_core` | AT-EXT-09 |
| FR-721 | MUST | pairing: the user retypes exactly 6 digits and nothing more | 022 | TASK-100/105 | `secure_remote`, CLIENT | AT-EXT-14 |
| FR-722 | MUST | 6-digit code from `esp_random`, 120 s, single-use, format `ddd ddd`; the whole window ≤ 180 s (constant) | 022 | TASK-100 | `secure_remote`, `ui_views` | AT-EXT-13/15 |
| FR-723 | MUST | mDNS discovery of `_kvmpad._tcp` (TXT `name`, `v`, `pair`); IP only as a fallback | 022 | TASK-080/105 | Wi-Fi (ADR-015), CLIENT | AT-EXT-14 |
| FR-724 | MUST | client name = Windows hostname, without retyping | 022 | TASK-105 | CLIENT | AT-EXT-14 |
| FR-725 | MUST | Accept/Reject screen with the client name, 30 s timeout, LOCAL always cancels | 022 | TASK-070C | `ui_views`, `secure_remote` | AT-EXT-14/15 |
| FR-726 | MUST | Paired clients: list, Remove, Remove all; factory reset clears them; revoke takes effect immediately | 022 | TASK-070C/090 | `ui_views`, `config_store`, `secure_remote` | AT-FAULT-10 |
| FR-727 | SHOULD | full pairing ≤ 60 s | 022 | TASK-100/105 | — | AT-EXT-14 |
| SEC-101 | MUST | PAKE: candidate EC-JPAKE P-256 (mbedTLS); alternative SPAKE2(+); interop vectors | 022 | TASK-100/105 | `secure_remote`, CLIENT | AT-EXT-13 |
| SEC-102 | MUST | channel binding: TLS certificate hash + client ID in the PAKE transcript; pinning | 022 | TASK-100/105 | `secure_remote`, CLIENT | AT-EXT-13/15 |
| SEC-103 | MUST | unverified TLS only for `pair.*` within an open window; 0 status/HID | 022 | TASK-100 | `secure_remote`, `remote_protocol` | AT-EXT-16 |
| SEC-104 | MUST | 3 attempts/window → close; 3 windows/10 min → 10 min lockout; counters | 022 | TASK-100/120 | `secure_remote`, `diagnostics` | AT-EXT-15 |
| SEC-105 | MUST | code/key/token never in logs; DEVICE keeps the SHA-256 of the token; CLIENT → DPAPI | 022 | TASK-100/105 | `secure_remote`, `config_store`, CLIENT | AT-SEC-08 |
| SEC-106 | MUST | pairing only with Remote ON + `wifi:CONNECTED`; endpoint only within the window | 022 | TASK-100 | `secure_remote` | AT-EXT-16 |
| FR-731 | MUST | confirmation policies = settings in Settings on DEVICE, NVS, safe defaults | 023 | TASK-070C/090 | `ui_views`, `config_store` | AT-EXT-18 |
| FR-732 | MUST | `pair.require_accept` ON/OFF (ON by default); OFF → toast with `Remove` for 10 s | 023 | TASK-070C/100 | `ui_views`, `secure_remote` | AT-EXT-21 |
| FR-733 | MUST | `control.approval`: always (default) / trusted / auto; `trusted` flag per client | 023 | TASK-070C/100 | `ui_views`, `secure_remote`, `config_store` | AT-EXT-20 |
| FR-734 | SHOULD | `shortcuts.confirm_alt_f4` (OFF by default, after the FR-310 tests) | 023 | TASK-070C | `ui_views` | AT-UI-02 |
| FR-735 | MUST | the active policy visible in the CLIENT status details | 023 | TASK-070C | `ui_components`, `ui_views` | AT-UI-04 |
| FR-736 | MUST | factory reset / corrupted NVS → safe defaults; migration never relaxes a policy | 023 | TASK-090 | `config_store` | AT-EXT-18 |
| SEC-107 | MUST | settings only from the local UI; no `settings.*` in the protocol; snapshot without policies | 023 | TASK-100 | `remote_protocol`, `secure_remote` | AT-EXT-19 |
| FR-741 | MUST | `Settings → Keyboard → Host layout`: ids `layout(variant)` from xkeyboard-config `base.xml`, default `us`, NVS | 024 | TASK-071/090 | `kbd_layouts`, `ui_views`, `config_store` | AT-EXT-22/23 |
| FR-742 | MUST | a layout changes only legends, the geometry family (ANSI/ISO/JIS/ABNT2/KR) and IME keys; hint "set the same layout on the HOST" | 024 | TASK-070B/071 | `ui_views`, `kbd_layouts` | AT-EXT-23 |
| FR-743 | MUST | keys stay usages; no keymap derived from legends; the DEVICE emits no Unicode through the plain key path | 024 | TASK-070B | `hid_core`, `ui_views` | AT-KEY-01 |
| FR-744 | SHOULD | "Switch HOST layout" shortcut per host OS (Win+Space / Ctrl+Space / Super+Space) | 024 | TASK-070C | `ui_views` | AT-UI-02 |
| FR-745 | MUST | CLIENT type text uses the generated tables; active layout from the read-only snapshot field `host_layout` | 024 | TASK-071/105 | CLIENT, `remote_protocol` | AT-EXT-24/25 |
| FR-746 | MUST | on-device composition: JP flick + romaji with kana→kanji, zh pinyin with candidates, KO 2-set algorithmic; the HOST IME is never required; in the main release | 024 | TASK-072 | `ime_engine`, `ui_views` | AT-EXT-26/27 |
| FR-747 | MUST | Unicode delivery modes `linux` / `macos` / `windows-hex` / `wincompose` / `host-ime`, chosen in Settings, setup hint per mode | 024 | TASK-071/072 | `kbd_layouts`, `ui_views`, `config_store` | AT-EXT-27 |
| FR-748 | MUST | one path: typed intents → arbiter; cancellable burst within the 03 §6.5 budget; `RELEASE_ALL` on cancel / mode switch / USB or owner loss | 024 | TASK-050/072 | `hid_core` | AT-EXT-27 |
| FR-749 | SHOULD | CLIENT type text sends out-of-table characters through the active delivery mode instead of skipping | 024 | TASK-105 | CLIENT | AT-EXT-24 |
| TECH-077 | MUST | offline generator on a Linux host; pinned xkeyboard-config (newest stable) + keycodemapdb; committed outputs; CI regenerates with an empty diff | 024 | TASK-071 | `tools/kbd_layouts`, `kbd_layouts` | AT-EXT-22 |
| TECH-078 | MUST | pre-rendered legend glyph subsets (Noto, `lv_font_conv`); shaping scripts best-effort; tables + fonts ≤ 1.5 MiB | 024 | TASK-071/120 | `ui_theme`, `kbd_layouts` | AT-EXT-22, AT-PERF |
| TECH-079 | MUST | every `base.xml` layout that fits the five geometry families; Expat / Expat-Open-Group sources only; `THIRD_PARTY_NOTICES.md` | 024 | TASK-071 | `kbd_layouts` | AT-EXT-22 |
| TECH-080 | MUST | engines OpenWnn + PinyinIME (Apache-2.0), in-house Hangul composer; spike measures flash / PSRAM / latency; no GPL | 024 | TASK-072 | `ime_engine` | AT-EXT-26 |
| TECH-081 | MUST | read-only `ime_data` partition shared by both OTA slots; microSD fallback; never downloaded at runtime | 024 | TASK-000/072 | partition table, `ime_engine`, `update_manager` | AT-FAULT-07 |
| TECH-082 | MUST | three-line license header (SPDX, copyright, Required Notice) in every authored source file; exemptions and third-party rule per ADR-025 | 025 | every task | all | AT-EXT-28 |
| TECH-083 | MUST | `tools/check_license_headers.py` blocking in CI and in the PR checklist; foreign SPDX ids must be listed in `THIRD_PARTY_NOTICES.md` | 025 | TASK-000 | `tools`, CI | AT-EXT-28 |
| TECH-084 | MUST | Required Notice + SPDX id as a retained string in the firmware image (`strings app.bin`); CLIENT `LegalCopyright` resource + About dialog | 025 | TASK-000/105 | `main`, CLIENT | AT-EXT-29 |
| FR-751 | MUST | `Settings → About`: firmware version, license + Required Notice, third-party notices generated from `THIRD_PARTY_NOTICES.md` at build time | 025 | TASK-070C | `ui_views` | AT-EXT-29 |

ADR-021 (native Windows CLIENT) — ACCEPTED 2026-11-01, resolves ADR-002 (variant A). TASK-105 (CLIENT, `clients/windows/`)
starts after TASK-100; the 05 §4 gate (`release_all` + epoch + HIL USB before any remote control) applies unchanged.

## Changes to the 03 §3 contract (to be introduced in TASK-050, in line with 03 §9)

New values appended **at the end** of `kvm_hid_action_type_t`:
`KVM_HID_CONSUMER_DOWN`, `KVM_HID_CONSUMER_UP` (ADR-018), `KVM_HID_KEEPALIVE` (ADR-020).

The 03 §9 rules apply: item 2 — changing the USB descriptor requires a HOST compatibility matrix and an enumeration test (ADR-018);
item 4 — a new HID source requires an update to `CONTROL_OWNER` and concurrency tests (ADR-020);
item 3 — the new `pair.*` remote events from ADR-022 require a whitelist, validation, a threat model, fuzzing (AT-EXT-17) and a
"no USB without owner" test (AT-EXT-16); ADR-018/019 do not add remote events (they use `input/shortcut` and `keyboard.key`);
item 3 — the read-only snapshot field `host_layout` from ADR-024 requires whitelist validation (an enumerated id, never free
text) and fuzzing (AT-EXT-25); ADR-024 adds no HID source (composition is `LOCAL` input) and no descriptor change.

ADR-024 changes the v1 scope statement of 01 §1.2 by owner decision: an on-screen keyboard with on-device IME for Japanese,
Simplified Chinese and Korean is **in** the main release; Unicode reaches the HOST through per-OS delivery modes (FR-747).
