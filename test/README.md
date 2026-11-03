# `test/`

Tests according to `docs/spec/04-TEST-PLAN-AND-ACCEPTANCE-CRITERIA.md`.

| Directory | Layer | Contents |
|---|---|---|
| `host_unit/` | L1 | deterministic FSM/HID/protocol/NVS tests (native C, sanitizers) |
| `fuzz/` | L2 | touch sequences + remote frames; record the seed and the reproducer |
| `hil/` | L3–L4 | pyusb/hidapi/uinput harness, enumeration, traces |
| `ui_store/` | L1 | TASK-060B |
| `ui_nav/` | L1/SDL | TASK-060C |
| `ui_touch/` | L1/L2 | TASK-060E |
| `ui_components/` | L1 | TASK-060D; the KVMFONT1 hanzi font loader `ui_font_blob` (hand-built blobs, every truncation, 3000 random corruptions, and the real blob when configured with `-DKVM_HANZI_BLOB=<path>`) |
| `net_status/` | L1 | TASK-080 Wi-Fi link state machine |
| `config_store/` | L1 | TASK-090 settings record, migrations, write policy |
| `ui_views/` | L1 | TASK-070A-C views and the product shell `ui_app` on real LVGL; the KO and ZH composition layers (ZH through a fake decoder) |
| `remote_protocol/` | L1/L2 | TASK-100 protocol v1 validator and session layer, frame fuzzer (`../build-host-remote`) |
| `pairing/` | L1 | TASK-100 pairing core, EC-JPAKE through PSA, interop vectors in `vectors/` (`../build-host-pairing`) |
| `update_manager/` | L1 | TASK-110 OTA state machine, image inspection, health check (`../build-host-ota`) |
| `ime/` | L1 | TASK-072 Hangul 2-set composer (all 11172 syllables round trip) and FR-747 Unicode delivery sequences, `ime_data` image directory parser (`../build-host-ime`) |
| `ime_pinyin/` | L1 | TASK-072 PinyinIME decoder through `ime_pinyin` on the pinned dictionary (vectors, refusals, 2000 random inputs under ASan) and the open/close leak regression; 32-bit target only (`../build-host-pinyin`, i686 clang) |
| `ime_wnn/` | L1 | TASK-072 OpenWnn Japanese conversion through `ime_wnn` on the dictionaries of the `ime_data` image (conversion vectors, romaji edge cases, partial choices, refusals, 2000 random inputs under ASan + UBSan) and the heap regression; builds for 64-bit and 32-bit alike (`../build-host-wnn`, `../build-host-wnn32`) |
| `../simulator/` | L1 | TASK-070D product shell headless at 1024×600, 39 goldens, AT-UIFW-01/02/04/08/09/10 (`../build-sim`) |
| `../clients/windows/test/` | L1 | TASK-105 CLIENT core (`dotnet test clients/windows/KVMpad.Client.sln`, NuGet cache under `../.tools/nuget`); `LiveDeviceTests` run only with `KVMPAD_LIVE_HOST` + `KVMPAD_LIVE_PIN` set (L3, against a board) |
| `hil/remote_probe.py` | L3 | TASK-100 transport from the LAN: mDNS, TLS pin, admission refusals, no plain listener |
| `hil/hid_capture_windows.py` | L3 | HOST-side Raw Input capture for the bench `hid probe` (AT-HID-02/03/04) |
| `reports/` | — | test reports (04 §12 format) |

Without a physical board, L3–L6 results are always `NOT_RUN_HW` — never `PASS_HW`.
