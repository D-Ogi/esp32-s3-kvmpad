# Persistent SSH host identity

The board listener calls `kvm_ssh_identity_open_nvs(&key, sha256)` from its single owner task after NVS and libssh initialization. On success, pass `key` to `ssh_bind_options_set(bind, SSH_BIND_OPTIONS_IMPORT_KEY, key)`. The bind owns the key if that call succeeds. Free the key on error. The 32 digest bytes belong in the authenticated TLS hello.

An absent `kvmpad_ssh` namespace triggers first provisioning. The listener uses an Ed25519 private key after committing and reading back the `host_v1` blob. Later loads open NVS read-only. If the namespace exists and the record is missing or corrupt, SSH stays unavailable. An interrupted first write can also leave an empty namespace. Existing pairings and TLS remain available to the rest of the application. Protection of the private record at rest depends on the board's NVS encryption configuration.

For the host test, build the pinned upstream libssh host library as described in the neighboring wire fixture. Point CMake at the same source and build directories.

```sh
cmake -S test/ssh_control/identity -B "$OUT/identity" -G Ninja -DLIBSSH_SOURCE="$SRC" -DLIBSSH_BUILD="$OUT/upstream"
cmake --build "$OUT/identity"
ctest --test-dir "$OUT/identity" --output-on-failure
```

The fixture runs the production provisioning path with an in-memory NVS implementation and real libssh key operations. Independent provisioning creates distinct keys. Reopening the same store reproduces its public fingerprint and private key without rewriting the record. The test alters each stored byte, truncates at every offset, checks an invalid key with a valid CRC and injects failures during open, read, write, commit and readback. A separate Python check computes SHA-256 from the exported public SSH blob. All private key material stays in process memory.
