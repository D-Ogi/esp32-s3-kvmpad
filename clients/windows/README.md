# KVMpad Windows CLIENT

The native Windows application that controls the HOST remotely through a KVMpad device over Wi-Fi
(ADR-021). It speaks protocol v1 over WSS with certificate pinning; it never touches USB itself.

Glossary, as the specification uses the terms: the **DEVICE** is the KVMpad board, the **HOST** is the
computer the board is plugged into over USB, and the **CLIENT** is this application.

Stack, dependencies and the decisions behind them: `docs/ADR/ADR-028-windows-client-stack.md`.
The pairing protocol itself is `docs/ADR/ADR-029-pairing-wire-format-and-pake.md`.

## Layout

| Path | Target | Contents |
|---|---|---|
| `src/KVMpad.Client.Core` | `net9.0` | Protocol v1 codec and session state machine, input rate limiter and coalescing, certificate pinning, scan-code → HID usage map, capture ledger, type text, mDNS discovery, the pairing run. No UI and no NuGet dependency; its one native dependency is the pairing library below, loaded by name. |
| `layouts/kbd_layouts.json` | data | The generated host layouts of ADR-024 (581 layouts, about 4.5 MB), produced by `tools/kbd_layouts/gen_layouts.py` — the same file the device compiles into its firmware. Read-only here: never edited by hand, and regenerated only by that tool. Both the application and the test project copy it to `layouts/kbd_layouts.json` beside their binary, which is where `HostLayoutCatalog` looks for it. |
| `native/` | Windows x64 | `kvmpad_pairing.dll`: the pairing core of ADR-029 — `components/secure_remote/pairing/**` — compiled against the TF-PSA-Crypto of the pinned ESP-IDF v6.1 tree, plus a flat C ABI for P/Invoke. Also `kvmpad_pairing_test.dll`, a test fixture that is never shipped. |
| `src/KVMpad.Client.App` | `net9.0-windows` | WinForms window, tray icon, dialogs, and the Win32 layer: `WH_KEYBOARD_LL`, Raw Input, session/power notifications, Credential Manager. |
| `test/KVMpad.Client.Core.Tests` | `net9.0` | xUnit tests for everything in Core. No test contacts a device or any network service; the TLS tests complete a real handshake on the loopback interface against a certificate the test itself creates. |

## Build and test

There are two build steps and the native one comes first, because the managed tests load its output.

Nothing may be installed on drive `C:` or globally, so the package cache and the `dotnet` CLI home are
pointed outside the repository before any command (project rule 2 and `CONTRIBUTING.md`). `bin/` and `obj/`
stay inside this directory and are ignored.

### 1. The native pairing library

ADR-029 compiles one pairing implementation twice, so this builds the firmware's own
`components/secure_remote/pairing/**` sources against the crypto the firmware links. It needs CMake, Ninja
and the llvm-mingw toolchain already in `../.tools/`; the CMake build directory stays outside the repository
and the two DLLs land in `native/bin/`, which is gitignored.

```bash
export PATH=/o/Praca/KVMpad/.tools/llvm-mingw-20261020-ucrt-x86_64/bin:$PATH
cmake -S clients/windows/native -B ../build-win-pairing -G Ninja \
      -DCMAKE_C_COMPILER=x86_64-w64-mingw32-clang \
      -DMBEDTLS_ROOT=O:/Praca/KVMpad/.tools/esp-idf-v6.1/components/mbedtls/mbedtls
cmake --build ../build-win-pairing
```

Two artifacts come out, and the split is deliberate (ADR-028 §4):

- `kvmpad_pairing.dll` — what the application ships. The CLIENT half of the exchange only, drawing randomness
  from `BCryptGenRandom`.
- `kvmpad_pairing_test.dll` — **a test fixture; never ship it.** It adds a driver for the DEVICE state
  machine and the seeded generator from `test/pairing/det_rng.c`, which is what makes the ADR-029 interop
  vectors reproducible. Both belong in a test and neither belongs in an application that controls someone's
  computer, so they are kept apart at build time rather than behind a runtime switch. The shipping DLL
  exports no `kvmpad_test_*` entry point and does not compile `det_rng.c` at all; see ADR-028 §4 for what
  that does and does not guarantee about the DEVICE code itself.

`kvmpad_pairing.dll` has to sit beside `KVMpad.Client.exe` at run time. Without it the application starts and
can still connect to an already-paired device; only pairing is unavailable, and it says so with the build
command rather than failing with a bare loader error.

### 2. The managed solution

PowerShell:

```powershell
$env:NUGET_PACKAGES = 'O:\Praca\KVMpad\.tools\nuget'
$env:DOTNET_CLI_HOME = 'O:\Praca\KVMpad\.tools\dotnet-home'
$env:DOTNET_SKIP_FIRST_TIME_EXPERIENCE = '1'
$env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
$env:DOTNET_NOLOGO = '1'

dotnet build clients/windows/KVMpad.Client.sln -c Release
dotnet test  clients/windows/KVMpad.Client.sln -c Release
```

Both commands run from the repository root. Required: the .NET SDK 9.0 (built and tested with 9.0.308) and
the `Microsoft.WindowsDesktop.App` 9.0 runtime, which the Windows SDK installer already provides.

A single test class, while narrowing something down:

```powershell
dotnet test clients/windows/test/KVMpad.Client.Core.Tests/bin/Release/net9.0/KVMpad.Client.Core.Tests.dll `
  --filter "FullyQualifiedName~PairingTests"
```

## What does not work yet

- **Nothing has run against a board or a real device on a LAN.** Every hardware and network result is
  `NOT_RUN_HW`: the device's acceptance of the `Authorization` header, mDNS against a real responder, a real
  pairing window, and whether typed text arrives correctly on the HOST.
- Pairing is implemented end to end against the real EC-JPAKE and is proven against the four ADR-029 interop
  vectors in-process, but it has never spoken to a device.
- **Advanced → Connect manually** exists for development: a token and a certificate fingerprint obtained some
  other way, no pairing run.
- There is no layout picker in the window yet. The layout follows the device's `host_layout` when it is
  published, and otherwise the `HostLayoutId` setting, which has to be edited in the settings file by hand.

## Host layouts

`HostLayoutCatalog` (Core, `TypeText/HostLayoutCatalog.cs`) indexes `layouts/kbd_layouts.json` once — id, name,
geometry and the two flags per entry, plus the byte range of that entry — and builds a character table only for
the layout actually in use, so the 4.5 MB file costs one read and no bulk deserialization. A character is one
key press, or two where the layout reaches it through a dead key; `TypeTextPlanner` presses and releases each
of them in turn, so any prefix of a run is still balanced. The level the generator records is the modifier
combination that reaches the character: bit 0 Shift, bit 1 AltGr (the right Alt usage).

If the file is missing or is not the file this client expects, the catalog stays empty, says so in one sentence
that never quotes the file, and the layout in use can type only the keys every layout shares (space, tab,
Enter). Everything else is reported as unavailable rather than guessed at (FR-702).

## Rules that apply here

- The CLIENT never emits USB HID. It sends typed intents; the DEVICE's `InputArbiter` and `HidScheduler`
  decide what reaches the HOST.
- No `ws://`, and no setting that skips certificate verification (TECH-041).
- Keystrokes, typed text, usages, tokens and pairing codes are never written to a log, a file or an exception
  message (TECH-046, SEC-105, FR-705). Diagnostics carry counts.
- Every exit from capture mode releases what is held, in reverse press order.
- Feedback-loop guard: when the KVMpad is plugged into the CLIENT machine itself, everything the client forwards
  comes back from the KVMpad as local input. The client therefore drops Raw Input from a KVMpad's own mouse, refuses
  capture while any KVMpad (USB `VID_303A&PID_400C`, `KvmpadUsbIdentity`) is attached to the CLIENT machine, and stops
  capture (`ReleaseReason.LoopGuard`) when one is plugged in during capture. The keyboard hook cannot tell devices
  apart, so refusing capture is the only safe answer. Pairing, status and type text are not affected.
- The pairing protocol is not reimplemented here. `components/secure_remote/pairing/**` is read-only from
  this directory: `native/` compiles those sources, and a change needed there is a change to TASK-100, not a
  local patch. The same goes for `test/pairing/`, whose fixture and vectors the native test DLL reuses.
- Every authored source file carries the three-line license header (ADR-025), checked by
  `python -I tools/check_license_headers.py`. `KVMpad.Client.sln` has no comment syntax and is exempt as a
  generated file.
