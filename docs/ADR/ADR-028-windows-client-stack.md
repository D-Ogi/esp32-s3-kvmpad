# ADR-028 — Windows CLIENT stack, authentication handshake and pairing-crypto interop
Status: ACCEPTED (owner, 2026-11-02)
Date: 2026-11-02
Approver: project owner (decision of 2026-11-02, accepted as proposed)
Task: TASK-105 (`clients/windows/**`)
Superseded in part by: **ADR-029** owns the pairing wire format, the PAKE, the channel binding and the token
sealing. Everything this ADR said about those is historical; §4 below now records only how the CLIENT reaches
that one implementation. The stack decision (§1) and the authentication handshake (§2, §3) stand.

## Context

ADR-021 resolved ADR-002 to variant A — a native Windows application as the first and only CLIENT in v1 — and
left "language/stack to be chosen in that task, with its own ADR". This is that ADR. It also settles three
things the firmware side needs in writing, because `remote_protocol` and `secure_remote` are being built in
parallel and both ends must agree:

1. how the pairing token is presented on the WebSocket handshake (03 §6.1 says only "not sent in a query string");
2. what the CLIENT needs from the pairing PAKE — which ADR-029 then specified, against the caller written here;
3. where the certificate pin comes from and what the CLIENT does when it does not match.

Constraints that shaped the choices: the workspace must not install anything on drive C: or globally; the
.NET SDK 9.0.308 is already present; the CLIENT has to install a system-wide keyboard hook and read Raw Input,
which rules out a sandboxed runtime; and ADR-025 requires the Required Notice in the binary and an About dialog.

## Options

### Language and UI

| | Option | Assessment |
|---|---|---|
| A | C++ / Win32 + a small UI toolkit | REJECTED — the hook, Raw Input and Credential Manager code is the same either way, and everything else (JSON, TLS, WebSocket, DPAPI, tests) would be hand-rolled or a new dependency. No upside for a client that is not on a hot path. |
| **B** | **C# on .NET 9, WinForms, a UI-free core library, xUnit** | **CHOSEN** — the SDK is already installed; `ClientWebSocket`, `SslStream` with a pinning callback, `System.Text.Json`, `AesGcm` and `ProtectedData` are all in the shared framework, so the runtime projects need zero NuGet packages; P/Invoke reaches every Win32 API this client needs; the protocol, mapping and release-all logic sit in a library with no UI or Win32 dependency and are unit-tested on a plain .NET host. |
| C | C# + WPF | Rejected for this client only: a device list, a status line, a text box and two dialogs gain nothing from XAML, and WinForms keeps the message-loop work (the hook and the Raw Input window) close to the surface. |
| D | Electron / web UI | REJECTED — a browser runtime for a HID client re-introduces the certificate problem ADR-021 removed, and ships a renderer this application has no use for. |
| E | Rust | Rejected on ecosystem grounds for v1: TLS, WebSocket and JSON would each be an external crate to pin and audit, against a framework that already ships them. |

### Authentication on the handshake

| | Option | Assessment |
|---|---|---|
| A | Token in the WebSocket URL query string | PROHIBITED by 03 §6.1 outright. |
| **B** | **`Authorization: Bearer <token>` on the HTTP upgrade request** | **CHOSEN** — the device can refuse an unauthenticated peer with a 401 before any WebSocket state machine exists, which is the earliest possible fail-closed point; it is a standard header, so it needs no custom framing; and it keeps the token out of the `hello` payload, which is ordinary protocol data the device may echo into diagnostics. `ClientWebSocket.Options.SetRequestHeader` sets it. |
| C | Token in the first `hello` payload | Rejected — the device would have to run an unauthenticated WebSocket connection and a JSON parser before deciding whether the peer may talk to it at all, widening the attack surface of AT-SEC-04 for no benefit. |
| D | TLS client certificate | Deferred — stronger, but it means the DEVICE issuing and tracking a certificate per client and the CLIENT holding a private key, where ADR-022 already produces a 32-byte shared token. Worth revisiting if a v2 adds multiple operator roles. |

### Pairing crypto interop

| | Option | Assessment |
|---|---|---|
| **A** | **One C core, compiled twice: an ESP-IDF component for the firmware and a native DLL for the CLIENT, both over the pinned mbedTLS sources, built with the llvm-mingw toolchain already in `.tools/`; reached from C# by P/Invoke** | **CHOSEN, and now built** — ADR-029 adopted this and the CLIENT implements it in `clients/windows/native/`. EC-JPAKE has no managed implementation, and one body of code means the interop vectors test that code rather than asserting two implementations agree. |
| B | A managed PAKE in C# beside the C one | Rejected. It was the fallback if the native build proved to be the harder half; it was not, so the fallback is moot. It would have doubled both what the vectors must prove and the review surface. |
| C | Variant D of ADR-022 (compare a 6-digit SAS on both screens) | No longer needed. It was the escape hatch if a PAKE proved unattainable on either side; ADR-029 §5 and the CLIENT's vector tests show it is attainable on both. |

## Decision

### 1. Stack

- **C# 13 on .NET 9** (`net9.0` for the library, `net9.0-windows` for the application), WinForms, x64.
- One solution, `clients/windows/KVMpad.Client.sln`, with three projects:
  - `src/KVMpad.Client.Core` (`net9.0`) — protocol v1 codec and state machine, the input rate limiter, certificate
    pinning, the scan-code → HID usage map, the capture ledger, "type text", mDNS, the pairing run, and the
    storage and pairing interfaces. **No UI, no Win32, no NuGet dependency**, so all of it is testable on a plain host.
  - `src/KVMpad.Client.App` (`net9.0-windows`, WinExe) — the window, the tray icon, the dialogs, and the Win32
    layer: `WH_KEYBOARD_LL`, Raw Input, `WM_WTSSESSION_CHANGE`, `WM_POWERBROADCAST`, Credential Manager.
  - `test/KVMpad.Client.Core.Tests` (`net9.0`, xUnit).
- **Dependencies.** The two runtime projects take none. The test project takes `xunit` 2.9.2,
  `xunit.runner.visualstudio` 2.8.2 and `Microsoft.NET.Test.Sdk` 17.11.1, pinned exactly in
  `Directory.Packages.props` with central version management; no floating ranges.
- **Nothing is installed on drive C:.** Restore writes to the folder named by `NUGET_PACKAGES`, and `dotnet`
  state to `DOTNET_CLI_HOME`; both point outside the repository and outside C:. `bin/` and `obj/` stay inside
  `clients/windows` and are ignored by `clients/windows/.gitignore`. The required environment is recorded in
  `clients/windows/README.md`.
- **ADR-025.** `Directory.Build.props` sets `<Copyright>` to the notice, which the SDK emits as both
  `AssemblyCopyrightAttribute` and the Win32 version-resource field `LegalCopyright` (TECH-084). `AssemblyInfo.cs`
  adds the Required Notice as assembly metadata, and `LicenseNotice` holds it as a constant so it lands in the
  string heap where `strings KVMpad.Client.exe` finds it. The About dialog shows it (FR-751).

### 2. Authentication handshake — what the firmware must accept

The control session is `GET wss://<host>:<port>/v1/ws` with:

| Header | Value | Notes |
|---|---|---|
| `Authorization` | `Bearer <token>` | The 32 bytes from `pair.accepted`, base64url without padding (43 characters). The DEVICE compares SHA-256 of the presented token against the stored hash **in constant time** (SEC-105 keeps only the hash). A wrong or missing token is **HTTP 401 with no body detail**, rate-limited per TECH-042, and never logged. |
| `Sec-WebSocket-Protocol` | `kvmpad.v1` | The DEVICE must echo it. The CLIENT aborts and does not open a session if the DEVICE selects nothing or something else, so an unversioned connection cannot be established by accident. |

The pairing endpoint is `GET wss://<host>:<port>/v1/pair`, subprotocol `kvmpad.pair.v1`, **with no
`Authorization` header** — there is no token yet — reachable only while the window is open and carrying
`pair.*` frames only (ADR-022 SEC-103, SEC-106). Outside the window: `ERR_PAIRING_EXPIRED`.

Both are `wss`. There is no code path in this client that opens `ws://`, and no setting that disables
certificate verification (TECH-041, AT-SEC-01). `hello` carries `client_name` and `capabilities` and **never a
token**.

Port: the CLIENT takes it from the mDNS `SRV` record and does not assume one. The manual fallback dialog
suggests 8443 only as a starting value in the field.

### 3. Certificate pinning

- The pin is **SHA-256 over the whole DER encoding** of the certificate the DEVICE presented during the pairing
  handshake — the same value the PAKE transcript commits to (SEC-102). It is stored with the token.
- A session is accepted **if and only if** that hash matches. `SslPolicyErrors` is ignored on purpose: the
  DEVICE's certificate is self-signed, so the chain never validates against a public root, and the pin is the
  identity. There is no "continue anyway".
- **Validity dates are not checked separately.** The pin covers the entire DER, including `notBefore` and
  `notAfter`, so a certificate that matches the pin is byte for byte the one the user approved on the DEVICE's
  screen. Re-checking expiry would add a date on which a working pairing stops working and exclude no attacker.
  If the DEVICE regenerates its key, the pin stops matching, the CLIENT refuses and says so, and the recovery
  is to pair again. **The DEVICE should therefore issue a long-lived self-signed certificate and keep it across
  reboots and OTA**, or every update costs every user a re-pairing.
- Token and pin live in the Windows Credential Manager as a generic credential whose blob is first sealed with
  DPAPI (`CurrentUser`, with application-specific entropy). Never a plain file (SEC-105). Persistence is
  `LOCAL_MACHINE`, so a token does not travel with a roaming profile.

### 4. How the CLIENT reaches the pairing protocol (ADR-029 owns the protocol)

ADR-029 settled the PAKE (EC-JPAKE on P-256 through PSA), the channel binding, the key schedule, the token
sealing and the `pair.*` wire format, and it adopted option A above. The channel-binding and `token_enc`
layouts this ADR originally proposed are **withdrawn**; ADR-029 §2 and §3 are normative, and the CLIENT
implements no layout of its own.

What remains an ADR-028 decision is only how the Windows application gets to that one implementation.

- **`clients/windows/native/`** builds the four `components/secure_remote/pairing/src/*.c` sources against
  the TF-PSA-Crypto of the pinned ESP-IDF v6.1 tree, with llvm-mingw from `.tools/`. The subproject flags,
  the `host_shim` include and the `MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG` arrangement are the same as in
  `test/pairing/CMakeLists.txt`, so firmware, host tests and CLIENT differ only in where entropy comes from:
  `esp_random()`, a seeded fixture, and `BCryptGenRandom` respectively.
- **Two artifacts, not one with a switch.** `kvmpad_pairing.dll` exports the CLIENT half only.
  `kvmpad_pairing_test.dll` adds the DEVICE state machine driver and the seeded generator of
  `test/pairing/det_rng.c` — both are needed to re-run the ADR-029 vectors, and neither belongs in an
  application that controls someone's computer. Keeping them in separate files is a build-time guarantee
  rather than a runtime check that could be flipped.

  Precisely what that guarantee is, because the distinction matters: the shipping DLL's export table holds 14
  `kvmpad_pair_*` entries and no `kvmpad_test_*` entry, so nothing outside it can drive the DEVICE side or
  seed a generator. `det_rng.c` is not among its sources at all, so the seeded generator its own header warns
  about cannot be present. The DEVICE state machine's *code* is a weaker claim: `--gc-sections` drops what no
  export reaches, but the FSM shares a translation unit with `kvm_pair_err_name()`, which the shim does call,
  so some of it is still linked in — unexported and unreachable, not absent. Removing the rest would mean
  splitting that translation unit in TASK-100's code, which is not worth a change to shared firmware sources
  for a library that is loaded by name and exports nothing to call it with.
- **A flat C ABI over an opaque handle** (`clients/windows/native/include/kvmpad_pairing_abi.h`). The managed
  side restates none of the core's structures, no key schedule and no size literal — every length comes from
  `kvmpad_pair_sizes()`. Restating any of it in C# would reintroduce exactly the drift ADR-029 removes. The
  shim holds no protocol logic and adds no stage checks of its own, because the core already enforces the
  call order and a second, slightly different gate would be a way for the two to disagree.
- **`IPakeHandshake`** (`Pairing/IPakeHandshake.cs`) stays, reshaped to ADR-029's call order, with
  `NativePakeHandshake` as its one implementation. It is not an extension point and not a place for a second
  PAKE; it exists so the transport driver in `PairingClient` is testable without a crypto library, and so the
  native boundary has one seam instead of being spread through the run.
- **Build dependency.** Pairing needs `kvmpad_pairing.dll` beside the application. Its absence is reported as
  `PairingUnavailableException`, whose message carries the build command, rather than a bare
  `DllNotFoundException` from the first call. The rest of the CLIENT — sessions, capture, type text — does not
  depend on it, so a build without the native step still connects to an already-paired device.
- **Client identifier.** ADR-029 §4 makes it 16 bytes. The CLIENT draws it once with
  `RandomNumberGenerator`, keeps it in its settings file, and regenerates it only if the file is missing or
  malformed. It is not a secret — it travels in `pair.hello` in the clear — but it is bound into the channel
  binding, so it has to be stable: a new value simply means the device sees a different client.
- **Pairing transport.** `wss://host:port/v1/pair`, subprotocol `kvmpad.pair.v1`, **no `Authorization`
  header**, and the one place in the CLIENT that accepts a certificate without a pin — which is what SEC-103
  permits and no more. The certificate is recorded rather than trusted: the run yields a token only if the
  PAKE transcript confirms that exact fingerprint, and the fingerprint that gets pinned is the captured one,
  never one taken from a later handshake (SEC-102). It is still `wss`; pairing over `ws://` does not exist
  here either.

### 5. Input safety

- The keyboard hook and Raw Input registration exist **only while capture is on** (ADR-021). Every exit from
  capture — the toggle, losing activation, a locked or logged-off Windows session, a suspend, application
  shutdown, a revoked lease, a dropped link, a cancelled "type text" run — goes through one release path that
  empties the ledger and sends an UP for everything held, in reverse press order.
- On a lost link or a revoked lease the UP events are **not** sent: there is no epoch and no socket, and the
  DEVICE runs `RELEASE_ALL` itself on loss of owner, USB or session. The ledger is cleared either way.
- The toggle key (Scroll Lock by default, configurable) is swallowed on both press and release, so it reaches
  neither machine.
- The CLIENT shapes its own input stream to **100 msg/s**, below the DEVICE's 120 (03 §6.5). `mouse.move`
  coalesces only with an adjacent move, so a button edge never changes position relative to the motion around
  it; a merged delta outside ±4096 is **split across several messages whose deltas sum to the original**, never
  clipped and never dropped. Key and button edges queue for budget and are never discarded.
- Nothing logs a keystroke, a usage, the text being typed, a token or the pairing code (TECH-046, SEC-105,
  FR-705). Diagnostic output is counts only. The crash handler prints an exception type, not a message.

## Consequences

- One more toolchain in the project (the .NET SDK), already installed, pinned by `global.json`-free SDK
  resolution to 9.0.308 on this machine; CI for the CLIENT needs a .NET 9 SDK image.
- `tools/check_license_headers.py` already recognizes `.cs`, `.csproj` and `.props`. It does **not** recognize
  `.sln`, whose format has no comment syntax; the solution file is treated as generated and stays exempt. This
  needs a line in the ADR-025 exemption list (see *Edits needed elsewhere*).
- An application with a global keyboard hook may be flagged by anti-malware; code signing is part of TASK-130
  (ADR-021).
- A pinned certificate that the DEVICE regenerates forces a re-pairing. Firmware must keep its key across
  reboots and OTA.
- No macOS, Linux or mobile client follows from this decision (ADR-021).
- **A second toolchain for the CLIENT: CMake, Ninja and llvm-mingw, for the native pairing library.** It is a
  real cost — the managed build alone no longer produces a complete application, and CI needs both steps —
  bought in exchange for not having a second PAKE implementation to keep in step. The dependency is confined
  to pairing: everything else in the CLIENT builds and runs without the native step.
- The CLIENT now depends on the pinned mbedTLS **sources**, not just on their behavior. Moving ESP-IDF to a
  version whose TF-PSA-Crypto layout differs breaks the CLIENT build at the same moment it breaks the
  firmware build, which is the intended coupling.
- The DEVICE's pairing register stores the SHA-256 of a token; the CLIENT stores the token itself, since it is
  the party that has to present it (SEC-105). The two sides of the register are deliberately asymmetric.

## Edits needed elsewhere (not made by TASK-105)

- `docs/ADR/README.md` — index row for ADR-028.
- `CONTRIBUTING.md` — the ADR summary table; and the ADR-025 exemption list should name `*.sln` explicitly.
- `docs/EXTENSIONS.md` — rows for the requirement ids this CLIENT implements, and the `TECH-084` row's
  CLIENT half is now satisfied for `LegalCopyright` and About.
- `docs/ROADMAP.md` — TASK-105 status.
- `docs/REPO-LAYOUT.md` — `clients/windows/` in the directory map, with the rule that CLIENT build output stays
  inside it and the package cache outside the repository.
- `test/README.md` — how to run `dotnet test` for the CLIENT and which `AT-` ids it covers, and that the
  CLIENT re-runs the ADR-029 interop vectors from `test/pairing/vectors/` through the native library.
- `docs/REPO-LAYOUT.md` — also `clients/windows/native/`, which compiles
  `components/secure_remote/pairing/**` for Windows. It reads those sources and `test/pairing/` and must not
  be mistaken for a fork of either; the dependency runs one way only.
- `THIRD_PARTY_NOTICES.md` — the CLIENT links TF-PSA-Crypto from the pinned mbedTLS tree, the same upstream
  the firmware already lists. Worth a note that the row covers both binaries rather than the firmware alone.

## Tests

| ID | Level | Covered here |
|---|---|---|
| AT-EXT-10 | unit | the US and Polish-programmers tables, every printable ASCII character, the AltGr pairs, unmapped characters counted |
| AT-EXT-12 | unit (HIL part separate) | cancel mid-run releases what was held; a lost lease stops the run |
| AT-EXT-13 | unit | all four ADR-029 interop vectors re-run through the native library and compared byte for byte — the four blobs, the transcript, both tags, the sealed token, the token and the stored hash — plus the wrong-code and substituted-certificate cases |
| AT-SEC-01 | unit | the session URI is always `wss`, no `ws://` path exists, a mismatched pin is refused over a real TLS handshake |
| AT-SEC-03 | unit | `seq` monotonic, no replay after reconnect, `ERR_STALE_EPOCH` stops input and releases |
| AT-SEC-04 | unit | malformed, over-long, deeply nested and wrong-version frames fail closed in both directions |
| AT-SEC-08 | unit + review | no token, code, usage or typed text in any description or diagnostic string |
| AT-CONN-04/05 | unit | "Connected" only from `CLIENT_CONNECTED`; an authenticated session without a lease sends no input |
| AT-CONN-07 | unit | `control.revoked` mid-drag drops the epoch and releases |
| AT-CONN-08 | unit | the heartbeat deadline fires inside 3 s and releases |
| AT-EXT-29 | unit + manual | the notice constants are exact; `LegalCopyright` and the About dialog carry them |

Everything that needs the board or a LAN is `NOT_RUN_HW`: AT-EXT-11, AT-EXT-14, AT-EXT-15, AT-EXT-16, AT-EXT-17,
AT-EXT-24, and the HIL halves of AT-SEC-* and AT-CONN-*.

## Migration

None — no CLIENT implementation existed.
