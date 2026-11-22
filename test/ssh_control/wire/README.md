# Direct SSH session wire test

This fixture uses the Windows OpenSSH client and the libssh source pinned by the target port (0.12.2, `07430deb9b97b751ec5ea5a7fc307f40bf042e0a`). The test binds a random IPv4 loopback port, creates disposable ECDSA and Ed25519 keys and checks the host against its own temporary known-hosts file. Its sink records typed intents in memory. No existing pairing, account keys, host trust or HID device is used.

Build from the repository root. Set `SRC` to the port's `libssh-mirror` directory and `OUT` to a private host build directory. Use a C compiler, CMake, Ninja, OpenSSL development libraries, Python, OpenSSH and ssh-keygen from the same local tool environment. On Windows use MinGW-w64 GCC with its matching bin directory on PATH.

```sh
cmake -S "$SRC" -B "$OUT/upstream" -G Ninja -DCMAKE_BUILD_TYPE=Release -DWITH_SERVER=ON -DWITH_EXAMPLES=OFF -DWITH_GSSAPI=OFF -DWITH_SFTP=OFF -DWITH_NACL=OFF -DWITH_ZLIB=OFF -DWITH_PCAP=OFF
cmake --build "$OUT/upstream" --target ssh
cmake -S test/ssh_control/wire -B "$OUT/fixture" -G Ninja -DLIBSSH_SOURCE="$SRC" -DLIBSSH_BUILD="$OUT/upstream"
cmake --build "$OUT/fixture"
ctest --test-dir "$OUT/fixture" --output-on-failure --repeat until-fail:20
```

The session handler takes an accepted libssh session on its owner's task. The caller supplies the host key, pairing lookup, bounded local approval and a session-specific lease. Its command callback checks that lease at dispatch. The handler checks it between packets and before each command, then releases once on every exit following an approval attempt. Keep the caller's context alive until the handler returns. The caller disconnects and frees the session.

Use a no-PTY restricted stream via `ssh -T kvmpad@BOARD` or the named subsystem via `ssh -T -s kvmpad@BOARD kvmpad`. Commands are newline-terminated and limited to 64 bytes including LF or CRLF. The input grammar is defined in `components/ssh_control/include/ssh_control.h`. Standard SSH exec commands, PTYs, arbitrary subsystems and forwarding are unavailable. A malformed, oversized or interrupted line terminates the control stream. On a clean, approved EOF the server writes and flushes the exit status, then gives the client up to one second to close the channel. A transport failure before the status flush fails the session. SSH has no acknowledgement for exit status, so the fixture separately checks the OpenSSH process exit code.

The wire cases cover an authenticated shell and subsystem, an empty approved stream, an unsigned key query, wrong user/key, local denial, revocation with or without further input, malformed/oversized/incomplete commands, unsupported channel requests and sudden disconnect while a key is held. Running this fixture verifies transport and callback behavior. End-to-end board pairing, local approval and physical HID require separate integration.
