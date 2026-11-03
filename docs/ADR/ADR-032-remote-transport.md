# ADR-032 — TLS and WebSocket transport for the remote session (`secure_remote` port layer)
Status: ACCEPTED (owner, 2026-11-02)
Date: 2026-11-02
Approver: project owner (decision of 2026-11-02, accepted as proposed)

## Context

ADR-021/022/023 settled *what* the remote feature does, ADR-028 settled what the CLIENT sends,
ADR-029 settled the pairing protocol and ADR-030 settled protocol v1. What nothing settles is how
the DEVICE actually listens: which TLS and WebSocket implementation, where the token is checked,
what certificate it presents, and how one state machine stays single-owner while the UI task, the
timer and the server task all reach into it.

`02-TECHNICAL-SPECIFICATION.md` §8 requires TLS with a per-device key (TECH-041, TECH-056),
authentication before any HID (TECH-040), a rate limit on failed attempts (TECH-042) and no
plaintext listener. `03-CONTRACTS-AND-PROTOCOLS.md` §6 fixes a 1024-byte text frame. Between those
two statements there are a dozen decisions an implementer would otherwise guess at, and several of
them are the difference between a fail-closed endpoint and an open one.

Everything below was checked against the pinned ESP-IDF v6.1 tree in `.tools/esp-idf-v6.1`, not
against documentation or memory; where a function is named here, it exists in that tree with that
signature.

## Decision

### 1. `esp_https_server` with its WebSocket support, not an own RFC 6455 framer

The pinned IDF already contains an RFC 6455 implementation sharing the TLS session with the HTTP
upgrade: masking, length encoding, fragmentation, the control opcodes and `httpd_ws_recv_frame()` /
`httpd_ws_send_frame_async()`. Writing a second framer would add an attack surface in the one place
an unauthenticated peer can reach, in exchange for nothing the requirements ask for.

Rejected: a minimal own framer on a raw `esp_tls` socket. It is a smaller dependency and a larger
liability — every masking and length-decoding bug would be ours, on the pre-authentication path.

What the component does **not** provide is authentication before the `101`, which is item 2.

### 2. The token is checked in the WebSocket **pre-handshake** callback

`httpd_uri_t::ws_pre_handshake_cb` (needs `CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT`) runs after the
request headers are parsed and **before** the server answers `101`. The whole admission decision
happens there: read `Authorization`, decode the 43-character base64url token, verify SHA-256 against
the register in constant time, check Remote / maintenance / slot availability. The callback then
either returns `ESP_OK` and lets the upgrade proceed, or sends a status and returns `ESP_FAIL`,
which closes the socket.

| Admission result | Response | When |
|---|---|---|
| `OK` | `101 Switching Protocols` | token known, Remote on, a free slot |
| `UNAUTHORIZED` | `401`, `WWW-Authenticate: Bearer`, body `unauthorized` | absent, malformed or unknown token; also every request during a TECH-042 cooldown |
| `FORBIDDEN` | `403`, body `forbidden` | Remote off, Wi-Fi down, maintenance, or no open pairing window on `/v1/pair` |
| `BUSY` | `503`, body `busy` | every control slot taken, or a pairing run already in progress |

No body carries detail. A peer must not be able to tell an unknown token from a disabled Remote, or
a closed pairing window from an expired code (SEC-105); the realm is omitted from the challenge for
the same reason.

The connection is registered with the session glue in `ws_post_handshake_cb`, not in the
pre-handshake callback, so a peer whose handshake fails after admission never occupies a slot. The
admission result is carried between the two callbacks in a single-slot variable, which is sound
because the server task handles one request at a time.

Rejected: checking the token in the URI handler, i.e. after the upgrade. The WebSocket state machine
and the JSON reader would then be reachable without a token, which is exactly what ADR-028 §2 chose
header authentication to avoid.

### 2a. The subprotocol is enforced, not merely echoed

`esp_http_server` answers `101` **without** a `Sec-WebSocket-Protocol` header when the client's offer
does not match the URI's `supported_subprotocol`; it does not fail the handshake. The CLIENT of
ADR-028 aborts when the echo is missing, so the connection dies either way — but it dies after the
upgrade, with a session already open on the DEVICE side.

The pre-handshake callback therefore requires the endpoint's own name (`kvmpad.v1` on `/v1/ws`,
`kvmpad.pair.v1` on `/v1/pair`) to appear in the offer, and answers `400` otherwise. This makes the
requirement symmetric — the DEVICE insists on the subprotocol exactly as the CLIENT does — and keeps
a non-conforming peer from ever occupying a slot. The endpoint's *kind* still comes from the URI, not
from the subprotocol, so no confusion between the two endpoints is possible either way.

### 3. `handle_ws_control_frames = true`

The endpoints take the control opcodes themselves: `PING` is answered with a `PONG` carrying the
same payload, a `CLOSE` is echoed and reported as `KVM_SRT_LOSS_CLIENT_CLOSE`, a `PONG` is dropped.

The reason is the loss vocabulary. With the flag off, the server answers `CLOSE` internally and the
handler never sees it, so a clean client close and a socket reset arrive at `close_fn`
indistinguishable — and the two produce different `control.revoked` reasons in diagnostics
(03 §6.4). Taking the opcodes costs about twenty lines and makes the distinction exact.

`BINARY` and `CONTINUE` frames are a protocol violation, not something to tolerate: protocol v1 is
text and one frame per message (03 §6.2), so a continuation frame is either a different client or an
attempt to split a document past the parser. Both close the connection with
`KVM_SRT_LOSS_PROTOCOL`. A text frame larger than 1024 bytes is refused by `httpd_ws_recv_frame()`
before its payload is read, which desynchronizes the stream, so the socket goes with it.

### 4. One instance, one mutex, sends inside the lock — no work queue

There is exactly one `kvm_srt_t` and one FreeRTOS mutex. Every core call and every socket write
happens with that mutex held. That is what keeps the server task and the UI task from interleaving
WebSocket frames on one socket, which would corrupt the stream rather than merely reorder it.
Socket reads happen before the lock is taken, so the hold time is one frame's processing plus, in
the pathological case of a stalled peer, one `send_wait_timeout`. That timeout is therefore also the
bound on how long a call from the UI task can wait, and it is set to 2 s
(`CONFIG_KVM_SRT_SOCKET_TIMEOUT_S`) rather than the IDF default of 5 s.

Rejected: marshalling every call onto the server task with `httpd_queue_work()`, the pattern the IDF
async-send example uses. It is the right answer for fire-and-forget sends, but the calls the UI makes
— pair accept/reject, client removal — need a return value, and waiting for a queued work item has
the same worst-case bound plus a queue that can be full and a control socket that can block.

Two consequences are written into the code:

- **Shutdown.** `httpd_stop()` makes the server task close every session, so `close_fn` would run
  while `kvm_srt_server_stop()` still holds the mutex. A flag is set before `httpd_stop()` and makes
  `close_fn` skip the core entirely; the core has already been told about every connection through
  `kvm_srt_drop_all(KVM_SRT_LOSS_SHUTDOWN)` while the sockets were still there to carry the last
  frame.
- **`httpd_sess_trigger_close()` is asynchronous** (it posts work), so the core's `close` hook can
  call it from inside a core call without reentering the lock.

### 5. The tick runs in its own task, not on `esp_timer`

`kvm_srt_tick()` must run about every 50 ms for the approval timeout, the heartbeat deadline and the
pairing deadlines, and it can send. Taking a mutex that may be held for up to 2 s is not something an
`esp_timer` callback may do — it would delay every other timer in the system. A 4 kB task at priority
4 (below the server task) sleeps 50 ms, takes the lock and calls the core. Blocking *that* task costs
nothing.

Rejected: `esp_timer` plus `httpd_queue_work()`. It works, but it adds the control-socket send to a
path whose only job is to tick, and the timer callback can still block in `sendto()` on a full queue.

### 6. `lru_purge_enable = false`

With LRU purging on — the `HTTPD_SSL_CONFIG_DEFAULT()` value — any peer that completes a TLS
handshake evicts the oldest session, which may be the one currently holding the HID lease. A remote
attacker could drop the operator by connecting. With it off, a connection past the socket limit is
refused at `accept()`.

The cost is the mirror-image denial of service: idle sockets held open block a real CLIENT. That is
bounded on both sides — a socket that completes TLS but never upgrades is closed after
`recv_wait_timeout`, and a WebSocket session that stops sending is closed by the protocol heartbeat
(03 §6.3) — and refusing a new peer is the safer failure of the two.

### 7. Certificate profile (what the CLIENT's pin will cover)

ADR-028 §3 pins SHA-256 over the whole DER and deliberately does not re-check validity, and it asks
the DEVICE for "a long-lived self-signed certificate kept across reboots and OTA". This is that
certificate:

| Field | Value | Why |
|---|---|---|
| Version | 3 | needed for extensions |
| Signature | ECDSA P-256 with SHA-256 | TECH-056; one curve on both ends |
| Serial | 8 random bytes, high bit cleared, never zero | a positive DER `INTEGER` |
| Subject = Issuer | `CN=<device name>`, e.g. `CN=KVMpad-7F3A` | self-signed; the same name the pairing screen and the mDNS record show |
| Validity | `20260101000000Z` .. `20760101000000Z`, fixed constants | the DEVICE has no RTC and may have no network time on first boot. ADR-028 §3 only holds if the dates cannot make a working pairing stop working |
| basicConstraints | not a CA | it signs nothing but itself |
| keyUsage | `digitalSignature`, `keyAgreement` | what a TLS 1.3 server key does |
| extendedKeyUsage | **absent** | see below |
| subjectKeyIdentifier / authorityKeyIdentifier | present when SHA-1 is configured in, otherwise omitted | cosmetic for a self-signed leaf; mbedTLS computes both with SHA-1 |

**No `extendedKeyUsage`.** A certificate with no EKU extension is valid for every purpose, while a
certificate that carries an EKU without `serverAuth` is rejected by Schannel before the CLIENT's
validation callback ever runs. Omitting the extension is therefore strictly safer than including it,
and adding it later would change the DER and break every existing pin.

The key is generated by `psa_generate_key()` with `PSA_KEY_USAGE_EXPORT`, copied into a PK context
with `mbedtls_pk_copy_from_psa()` long enough to write the SEC1 DER and sign the certificate, and
then destroyed in PSA. Both DER blobs (certificate ~450 B, key 121 B) are handed to an injected blob
store; the component has no NVS dependency.

Persistence is treated as part of generation: **a failed save is fatal.** A certificate that is not
persisted would change on the next boot and silently break every pin, which is worse than refusing
to start. Symmetrically, blobs that load but do not parse — or whose public keys disagree — are
reported as `ESP_ERR_INVALID_STATE` and **not** quietly regenerated; replacing the identity costs
every user a re-pairing, so that is the owner's decision through
`kvm_srt_identity_regenerate()`.

### 8. `conn_id` is a monotonic counter, never the socket descriptor

Descriptors are reused. A connection that is still closing and a new connection on the recycled
descriptor would collide in the session table, and the second would inherit the first's lease. The
adapter keeps a small descriptor-to-identifier table instead.

### 9. mDNS is an optional dependency

`port/kvm_srt_mdns.c` compiles to three stubs returning `ESP_ERR_NOT_SUPPORTED` when the managed
`espressif/mdns` component is absent (`__has_include("mdns.h")`), and the component declares the
dependency with `idf_component_optional_requires(PRIVATE espressif__mdns)`. Discovery is a
convenience: ADR-028 §2 has the CLIENT take the port from the `SRV` record but also offers a manual
address, so a device that cannot advertise must still pair and still serve. The TXT record is built
by the pairing core (`kvm_pair_mdns_txt`), so its keys and their order are decided in one place for
both ends, and `pair=1` appears only while a window is open (SEC-103). `kvm_srt_mdns_stop()` removes
the service but does not call `mdns_free()`, because the stack may be shared.

### 10. TECH-042 is a refusal, not a sleep

After `KVM_SRT_AUTH_FAIL_MAX` (5) consecutive failed admissions the endpoint answers `401` for
`KVM_SRT_AUTH_COOLDOWN_US` (30 s) without consulting the register. Delaying the response instead
would block the server task, which is the task every other connection and the tick depend on.

### 11. The pairing endpoint never becomes a protocol session

`kvm_rp_hooks_t::pair_frame` is wired to `NULL`. The pairing connection has no `remote_protocol`
session, no owner mirror and no intent sink; it has its own replay guard and its own server-to-client
`seq`. "Unverified TLS only for `pair.*`" is therefore a property of the wiring, not a check that
could be forgotten (SEC-103). One run at a time: a second peer on `/v1/pair` gets `503`.

## Required configuration

Added to `sdkconfig.defaults` by the integrator (none of these is an IDF default):

```
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT=y
CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y
CONFIG_MBEDTLS_X509_CREATE_C=y
CONFIG_MBEDTLS_ECJPAKE_C=y
CONFIG_MBEDTLS_SSL_PROTO_TLS1_3=y
```

`CONFIG_MBEDTLS_ECJPAKE_C` is `n` in the pinned preset, and without it `psa_pake_setup()` returns
`PSA_ERROR_NOT_SUPPORTED`, so pairing fails closed at the first round with `ERR_CONTROL_DENIED` and
no diagnosis pointing at the configuration. It is the one line of the six whose absence is silent.

`CONFIG_MBEDTLS_PK_WRITE_C`, `CONFIG_MBEDTLS_X509_CRT_WRITE_C` and `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`
are already on (the first two default to `y` once `X509_CREATE_C` is set, the third is in the current
defaults for Wi-Fi). The component's own options live in `components/secure_remote/Kconfig`.

Managed dependency for FR-723, in the component manifest `components/secure_remote/idf_component.yml`
(pinned to the version the build and the board were checked with; `dependencies.lock` records it):

```yaml
dependencies:
  espressif/mdns: "==1.14.1"
```

Integration (done in the firmware): `remote_protocol` and `secure_remote` are off `EXCLUDE_COMPONENTS` in the
root `CMakeLists.txt`; `main/remote_glue.c` maps the hooks onto `hid_dispatch` (REMOTE origin), `config_store`
(paired register, policy) and NVS (identity), and the device UI answers the prompts through a queue drained by
the glue task, so no LVGL callback waits for the transport mutex.

## Consequences

- The certificate profile in item 7 is now part of the CLIENT contract, because the pin covers every
  byte of it. Changing any field in item 7 after the first device ships means every user pairs again.
- The 2 s socket timeout of item 4 is the bound the UI must tolerate on a pairing or client-removal
  button press. If that proves visible in HIL, the fix is to make those calls asynchronous and drive
  the screen from `pair_event`, not to raise the timeout.
- Items 1, 2, 3 and 6 all depend on `esp_https_server` behavior that the IDF could change in a minor
  release. The pinned tree is v6.1; an IDF bump has to re-check that `close_fn` is still not
  overridden by `esp_https_server` and that the pre-handshake callback still runs before the `101`.
- Internal RAM cost is small by design (250 B of `.bss`/`.data`); the session state and the identity
  are in PSRAM through `EXT_RAM_BSS_ATTR`. The dominant cost is per-socket TLS memory, which is why
  `CONFIG_KVM_SRT_MAX_SOCKETS` defaults to 3 rather than the protocol ceiling of 5.
- If the owner rejects any item, the change belongs in this ADR and in `components/secure_remote`,
  not in the frozen specification.

## Relation to the earlier decisions

- **ADR-028** is satisfied as written; nothing here contradicts it. Items 2 and 7 are the DEVICE half
  of its §2 and §3, including the long-lived certificate its §3 asks for. Its §4 already withdrew the
  channel-binding and `token_enc` layouts in favor of ADR-029, so there is no conflict left to
  resolve. The one thing it needs that the pairing core did not provide is a decoder for the
  unpadded base64url token; that lives in `src/kvm_srt_auth.c` and rejects padding, the standard
  alphabet, whitespace and non-canonical trailing bits.
- **ADR-029** is normative for the `pair.*` wire format; `src/kvm_srt_pair_json.c` implements it and
  its error-code table is cross-checked against `kvm_pair_err_wire_code()` by a host test, so the two
  cannot drift.
- **ADR-030** is normative for protocol v1; this layer hands complete frames to it and never parses
  them itself.

## Tests

Host suites in `test/secure_remote/` (CMake + CTest, llvm-mingw, sanitized where the toolchain
allows): `test_srt_auth` (250 checks — bearer parsing, strict base64url), `test_srt_pair_json`
(164 checks — envelope, payloads, serializer, the ADR-029 §5 error table), `test_srt_session`
(328 checks — admission, TECH-042, the loss table, a full pairing run over real JSON against the
CLIENT driver, approval policies, FR-726 revocation, SEC-103/106, a no-secrets-on-the-wire sweep) and
`fuzz_srt_pair` (seeded mutation fuzzer over the pairing endpoint: no HID intent, no stored client,
no code echoed, every reply a well-formed v1 frame).

The `port/` layer is compile- and link-checked only, in an isolated project outside the repository,
in both the mDNS-present and mDNS-absent configurations. Its behavior — the TLS handshake, the `401`
before the upgrade, the pin a real CLIENT computes, mDNS discovery — is `NOT_RUN_HW` and belongs to
the HIL pass with a physical board and the Windows CLIENT.
