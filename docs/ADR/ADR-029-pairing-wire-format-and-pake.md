# ADR-029 — Pairing: PAKE algorithm, channel binding and `pair.*` wire format
Status: ACCEPTED (owner, 2026-11-02)
Approver: project owner (decision of 2026-11-02, accepted as proposed)
Date: 2026-11-02
Supersedes: nothing. Elaborates ADR-022 (which left SEC-101 open pending interop evidence).

## Context

ADR-022 fixed the pairing experience — one 6-digit code, physical acceptance, a 180 s window — and named
EC-JPAKE on P-256 as a *candidate* for SEC-101, with the final choice deferred to TASK-100 "with interop
evidence: identical test vectors on the firmware and client sides". It also left the concrete `pair.*`
payloads sketched rather than specified. This decision closes both.

Two constraints shape everything below.

1. **The CLIENT must reproduce the DEVICE bit for bit.** ADR-021 makes the only v1 CLIENT a native Windows
   application. A second, independent implementation of a PAKE is where interop bugs and security bugs come
   from, so the cheapest safe route is one implementation compiled twice: the pairing core is plain C over
   the PSA crypto API with no firmware dependency, and the CLIENT links the same sources and the same pinned
   crypto as a native DLL.
2. **The pinned library decides what is available.** ESP-IDF v6.1 ships mbedTLS 4.1.0 with the
   TF-PSA-Crypto 1.x layout. What that tree actually implements was read out of its headers, not assumed.

## Decision 1 — EC-JPAKE on P-256 through the PSA PAKE interface (SEC-101)

**Chosen:** `PSA_ALG_JPAKE(PSA_ALG_SHA_256)` over `PSA_PAKE_PRIMITIVE(PSA_PAKE_PRIMITIVE_TYPE_ECC,
PSA_ECC_FAMILY_SECP_R1, 256)`, driven through `psa_pake_setup` / `psa_pake_output` / `psa_pake_input` /
`psa_pake_get_shared_key`.

Evidence from the pinned tree:

| Finding | Where |
|---|---|
| J-PAKE is the only PAKE implemented, and only on secp256r1 with SHA-256 | `tf-psa-crypto/include/psa/crypto_extra.h`, note on `PSA_ALG_JPAKE` |
| SPAKE2+ is **not** implemented in this version | `tf-psa-crypto/docs/4.0-migration-guide/pake.md`: "Support for SPAKE2+ is likely to be added in a future version but is not there yet" |
| The legacy `mbedtls_ecjpake_*` interface is private in 4.x | its header is `tf-psa-crypto/drivers/builtin/include/mbedtls/private/ecjpake.h` |
| The built-in implementation accepts only the identities `"client"` and `"server"` | `tf-psa-crypto/drivers/builtin/src/psa_crypto_pake.c`, `jpake_client_id` / `jpake_server_id` |
| The interface changed between mbedTLS 3.6 and this version (password key passed to `psa_pake_setup`, hash folded into the algorithm, `psa_pake_get_shared_key` replaces `psa_pake_get_implicit_key`) | the same migration guide |

Rejected:

| | Option | Why not |
|---|---|---|
| A | SPAKE2+ (RFC 9383) | Not implemented in the pinned crypto. Taking it would mean adding a second crypto stack to the firmware or hand-writing a PAKE — both worse than using what is already pinned, audited and shipped. |
| B | Legacy `mbedtls_ecjpake_*` | Private in mbedTLS 4.x. Building against a private header makes the pinned-version upgrade path a maintenance hazard for no gain, since the PSA interface exposes the same primitive. |
| C | SPAKE2 (RFC 9382) | Same availability problem as A, and it lacks the asymmetric-storage property that made SPAKE2+ interesting in the first place. |
| D | Any hand-written construction | Forbidden by SEC-101. |

If a later pinned version adds SPAKE2+, revisiting this is a new ADR, not a silent change: the wire version in
§4 exists for exactly that.

**Consequence for the firmware build:** `PSA_WANT_ALG_JPAKE` is derived from `CONFIG_MBEDTLS_ECJPAKE_C`
(`components/mbedtls/port/include/mbedtls/esp_config.h`), and the ESP-IDF default preset sets
`CONFIG_MBEDTLS_ECJPAKE_C=n`. It has to be turned on in `sdkconfig.defaults`.

## Decision 2 — Channel binding through the password and the transcript (SEC-102)

SEC-102 requires the PAKE transcript to bind the SHA-256 of the DEVICE TLS certificate and the client
identifier. The J-PAKE identity fields are the natural place for that and are **not usable**: the pinned
implementation rejects any identity other than `"client"` and `"server"`. The binding therefore enters through
the password and is repeated in the transcript hash that every derived key depends on:

```
binding   = 0x01 || cert_sha256[32] || client_id[16] || len(client_name) || client_name
pw        = HKDF-SHA256(salt = "KVMpad-pair-v1/pw", ikm = code (6 ASCII digits), info = binding)
```

`pw` is imported as a `PSA_KEY_TYPE_PASSWORD` key, which is the documented way to pass "some value derived
from the password as required by a higher level protocol". A substituted certificate, a different client
identifier or a different client name produces a different password, so the exchange fails at key
confirmation and nothing is derived, stored or pinned.

HKDF over a 6-digit code is **not** a password hash and is not meant to be one: J-PAKE's security against
guessing comes from one guess per protocol run plus the SEC-104 attempt limit, not from the cost of the
derivation.

The DEVICE additionally compares the certificate hash the CLIENT reports in `pair.hello` against its own
**before** starting a run. A mismatch is refused immediately, so a certificate-substitution probe cannot burn
the legitimate user's three attempts.

## Decision 3 — Key schedule, explicit key confirmation and token sealing

```
Z         = the 65-byte J-PAKE shared point from psa_pake_get_shared_key(), 0x04 || X || Y
th        = SHA-256("KVMpad-pair-v1" || 0x01 || binding
                    || u16be(len) || device_round1 || u16be(len) || client_round1
                    || u16be(len) || device_round2 || u16be(len) || client_round2)
kc_device = HKDF-SHA256(salt = th, ikm = Z, info = "KVMpad-pair-v1/kc-device", 32)
kc_client = HKDF-SHA256(salt = th, ikm = Z, info = "KVMpad-pair-v1/kc-client", 32)
k_token   = HKDF-SHA256(salt = th, ikm = Z, info = "KVMpad-pair-v1/token", 32)
n_token   = HKDF-SHA256(salt = th, ikm = Z, info = "KVMpad-pair-v1/token-nonce", 12)
confirm_X = HMAC-SHA256(kc_X, th)                       X in {device, client}
token_enc = AES-256-GCM(k_token, n_token,
                        aad = "KVMpad-pair-v1/token" || th,
                        pt  = token[32])                -> 48 bytes, ciphertext || tag
```

Notes that matter for a second implementation:

- The blob order inside `th` is **canonical, not local**: DEVICE round 1, CLIENT round 1, DEVICE round 2,
  CLIENT round 2, whatever order the transport used. Both sides therefore hash the same bytes.
- Every label gets its own HKDF run. Nothing is cut out of one longer output, so a label can never be
  confused with a prefix of another.
- `Z` is the full uncompressed point and is only ever KDF input. The J-PAKE shared secret is not uniformly
  pseudorandom, so it never becomes a cipher key directly.
- The AEAD nonce is derived rather than drawn. `k_token` encrypts exactly one message in its lifetime, so a
  derived nonce cannot repeat, and it removes the last non-reproducible value from the exchange.
- Explicit key confirmation is required (RFC 8236 §5): after the two rounds J-PAKE gives only implicit
  authentication, and the DEVICE must not show an Accept screen for a peer that has not proved it holds the
  key. `PSA_PAKE_STEP_CONFIRM` exists in the interface but is not implemented for J-PAKE in the pinned tree,
  so confirmation is the HMAC above.
- **The CLIENT confirms first.** The DEVICE verifies before it reveals its own tag and before anything is
  stored, so a wrong code costs one attempt and produces nothing.

## Decision 4 — `pair.*` wire format

The envelope is the JSON v1 envelope of `03-CONTRACTS-AND-PROTOCOLS.md` §6.2 (`v`, `type`, `seq`,
`session_id`, `payload`); `lease_epoch` is absent, pairing has no lease. No binary framing is introduced: the
largest payload fits the 1024-byte frame limit of 03 §6.5 with room to spare, and keeping one envelope keeps
one parser and one validator.

**Round blobs.** A blob is a concatenation of length-prefixed chunks:

```
chunk  := u8 length | <length> bytes            1 <= length <= 65
round1 := chunk(X1.key_share) chunk(X1.zk_public) chunk(X1.zk_proof)
          chunk(X2.key_share) chunk(X2.zk_public) chunk(X2.zk_proof)      <= 330 bytes
round2 := chunk(X2S.key_share) chunk(X2S.zk_public) chunk(X2S.zk_proof)   <= 165 bytes
```

Key shares and Schnorr public keys are uncompressed P-256 points, exactly 65 bytes, `0x04 || X || Y`. A
Schnorr proof is a big-endian scalar of **at most** 32 bytes — the pinned implementation strips leading zero
bytes, so the length varies and the prefix is what makes the blob parseable. A blob with a zero-length chunk,
a chunk over 65 bytes, a truncation or trailing bytes is `ERR_BAD_FRAME`.

Blobs, confirmation tags and the sealed token travel **base64**, standard alphabet, padded, no embedded
whitespace. Hashes and identifiers travel **lowercase hex**.

| # | `type` | Direction | Payload | Notes |
|---|---|---|---|---|
| 1 | `pair.hello` | C→D | `v` (int, 1), `pake` (`"ecjpake-p256-sha256"`), `client_name` (UTF-8, ≤ 23 bytes), `client_id` (hex, 16 bytes), `cert_sha256` (hex, 32 bytes) | Refused outside an open window. `client_id` is a stable per-installation value the CLIENT generates once. |
| 2 | `pair.round1` | D→C | `blob` (base64 round1), `device_name` | Doubles as the acknowledgment of `pair.hello`. |
| 3 | `pair.round1` | C→D | `blob` (base64 round1) | |
| 4 | `pair.round2` | D→C | `blob` (base64 round2) | |
| 5 | `pair.round2` | C→D | `blob` (base64 round2) | |
| 6 | `pair.confirm` | C→D | `mac` (base64, 32 bytes) = `confirm_client` | The CLIENT proves first. |
| 7 | `pair.confirm` | D→C | `mac` (base64, 32 bytes) = `confirm_device` | Sent only after step 6 verified. |
| 8 | `pair.accepted` | D→C | `token_enc` (base64, 48 bytes), `device_name` | After `Accept`; with `pair.require_accept` OFF it follows step 7 immediately. |
| 8' | `pair.rejected` | D→C | `reason`: `timeout` \| `local_reject` \| `locked` | No detail that would help an attacker. |

The CLIENT may send steps 5 and 6 back to back; it holds everything it needs after step 4.

**Errors.** Refusals use the ordinary `error` message of 03 §6.4 with these codes:

| Situation | Code |
|---|---|
| No open window, code expired, 180 s cap reached, lockout active, another run holds the window, message out of sequence | `ERR_PAIRING_EXPIRED` |
| Malformed blob, wrong tag length, unknown `pake`, `v != 1`, missing `client_name` | `ERR_BAD_FRAME` |
| Key confirmation failed, certificate-hash mismatch, register full, internal failure | `ERR_CONTROL_DENIED` |

The first row deliberately collapses five distinct internal states onto one code: a probe must not be able to
tell a closed window from an expired code from a lockout.

**mDNS (FR-723).** Service `_kvmpad._tcp.local`; TXT keys in order `name` (the advertised DEVICE name),
`v=1`, `pair=1|0`, with `pair=1` only while a window is open.

## Decision 5 — Interop test vectors

`test/pairing/vectors/*.json` carry four recorded runs: a successful pairing with `require_accept` ON, the
same with it OFF, a wrong code, and a substituted certificate. Each records the inputs (code, certificate
hash, client identifier, client name) and every resulting byte: the four round blobs, the transcript hash,
both confirmation tags, the sealed token, the token and the hash the DEVICE stores.

The runs are reproducible because the test fixture replaces the random source — including the one PSA draws
the J-PAKE ephemeral scalars from — with a seeded generator. A live DEVICE and a live CLIENT draw real
randomness, so their blobs differ on every run; what the vectors pin down is the part a second implementation
can get wrong, namely how the tags, keys and sealed token follow from the blobs and the binding.
`test_pairing_vectors` re-runs all four and compares field by field, so a changed label or layout breaks the
build instead of breaking the CLIENT.

## Requirements covered

FR-721, FR-722, FR-723, FR-725, FR-726; SEC-101, SEC-102, SEC-104, SEC-105, SEC-106.
Tests: AT-EXT-13 (vectors, wrong code, certificate mismatch), AT-EXT-15 (limits and deadlines),
AT-EXT-16 (`pair.*` outside the window), AT-EXT-17 (malformed frames), AT-SEC-06, AT-SEC-08.

SEC-103 (unverified TLS only for `pair.*` inside an open window) is a transport obligation, not a pairing-core
one; the core exposes the window state the transport gates on.

## Consequences

- One implementation of the protocol, compiled for three targets. The CLIENT gains a build dependency on the
  pinned mbedTLS sources instead of a second PAKE implementation.
- `CONFIG_MBEDTLS_ECJPAKE_C=y` is required in the firmware configuration; it pulls in the P-256 elliptic
  curve code, which the TLS stack already needs.
- One J-PAKE run is the most expensive one-off computation in the product. It is measured on the host and
  estimated for the board in the TASK-100 report; it is a one-time cost during pairing and never on an input
  path.
- Changing any label, the blob layout or the transcript order is a wire break. `KVM_PAIR_WIRE_VERSION` and the
  `v` field in `pair.hello` exist so that such a change is refused rather than misinterpreted.
