# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Run stock OpenSSH with disposable identities against the simulated loopback HID sink."""
import argparse
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path


def run_case(folder, exe, env, name, *, channel=(), identity="client", data=b"", ok=False,
             approvals=0, commands=0, user="kvmpad"):
    scenario = "revoke" if name == "idle-revoke" else name
    with subprocess.Popen([exe, folder/"server", folder/"client.pub", scenario], env=env,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) as board:
        try:
            line = board.stdout.readline().strip()
            if not line.startswith("PORT "):
                raise RuntimeError(f"{name}: loopback listener failed ({line})")
            port = line.split()[1]
            hostkey = (folder/"server.pub").read_text("ascii").split()[:2]
            known = folder/"known_hosts"
            known.write_text(f"[127.0.0.1]:{port} {' '.join(hostkey)}\n", encoding="ascii")
            options = ["ssh", "-F", "none", "-T", "-o", "BatchMode=yes", "-o", "IdentitiesOnly=yes",
                       "-o", "IdentityAgent=none", "-o", "StrictHostKeyChecking=yes",
                       "-o", "UpdateHostKeys=no", "-o", "GlobalKnownHostsFile=none",
                       "-o", f'UserKnownHostsFile="{known.as_posix()}"', "-p", port,
                       "-i", str(folder/identity)]
            if name in ("subsystem", "wrong-subsystem"):
                options.append("-s")
            if name == "keepalive":
                options.extend(["-o", "ServerAliveInterval=1"])
            if name == "forward":
                options.extend(["-N", "-o", "ExitOnForwardFailure=yes", "-R", "0:127.0.0.1:1"])
            options.extend([f"{user}@127.0.0.1", *channel])
            if name == "keepalive":
                with subprocess.Popen(options, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                      stderr=subprocess.PIPE, env=env) as client:
                    try:
                        assert board.stdout.readline().strip() == "PEER_ALIVE"
                        client.communicate(timeout=5)
                        client_ok = client.returncode == 0
                    finally:
                        client.kill()
            elif name in ("disconnect", "idle-revoke"):
                with subprocess.Popen(options, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                      stderr=subprocess.PIPE) as client:
                    try:
                        client.stdin.write(data)
                        client.stdin.flush()
                        assert board.stdout.readline().strip() == "SINK_ACCEPTED"
                        if name == "idle-revoke":
                            assert client.wait(timeout=5) != 0
                    finally:
                        client.kill()
                        client.wait()
                client_ok = False
            else:
                client = subprocess.run(options, input=data, capture_output=True, timeout=24)
                client_ok = client.returncode == 0
            stdout, stderr = board.communicate(timeout=18)
            match = re.search(r"RESULT ok=(\d+) queries=(\d+) approvals=(\d+) commands=(\d+) releases=(\d+) held=(\d+) typed=(\d+)", stdout)
            if board.returncode or not match:
                raise RuntimeError(f"{name}: simulated server did not finish: {stdout} {stderr}")
            accepted, queries, grants, count, releases, held, typed = map(int, match.groups())
            if (client_ok, accepted, grants, count, releases, held, typed) != (
                    ok, int(ok), approvals, commands, approvals, 0, 1) or queries < 1:
                raise AssertionError(f"{name}: client_ok={client_ok}, server={match.group(0)}")
            diag = re.search(r"DIAG kex=1 query=(\d) query_ok=(\d) valid=(\d) auth=(\d) channel=(\d) subsystem=(\d) shell=(\d) end=(\w+)", stdout)
            assert diag and diag[8] != "missing", name
            if ok or name == "deny":
                assert diag.groups()[:5] == ("1",) * 5, (name, diag.groups())
                assert (diag[6], diag[7]) == (("1", "0") if name == "subsystem" else ("0", "1")), name
                assert diag[8] == ("ok" if ok else "approval"), name
            if name in ("unsigned-query", "unpaired", "wrong-user"):
                assert diag[1] == "1" and diag.groups()[2:7] == ("0",) * 5, (name, diag.groups())
            print(f"PASS {name} approvals={grants} typed_commands={count} releases={releases}", flush=True)
        finally:
            if board.poll() is None:
                board.kill()
                board.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("server", type=Path)
    parser.add_argument("library", type=Path)
    parser.add_argument("work", type=Path)
    args = parser.parse_args()
    env = dict(os.environ)
    env["PATH"] = str(args.library.resolve()) + os.pathsep + env["PATH"]
    stream = b"key down 4\r\nmouse move -2 3\nkey up 4\n"
    with tempfile.TemporaryDirectory(dir=args.work) as tmp:
        folder = Path(tmp).resolve()
        for name in ["server", "client", "unpaired"]:
            subprocess.run(["ssh-keygen", "-q", "-t", "ecdsa", "-b", "256", "-N", "",
                            "-f", str(folder/name)], check=True)
        shutil.copyfile(folder/"client.pub", folder/"public-only.pub")
        def test(name, **kwargs):
            run_case(folder, args.server.resolve(), env, name, **kwargs)

        test("shell", ok=True, approvals=1, commands=3, data=stream)
        test("keepalive", ok=True, approvals=1)
        test("empty-stream", ok=True, approvals=1)
        test("subsystem", channel=("kvmpad",), ok=True, approvals=1, commands=3, data=stream)
        test("wrong-subsystem", channel=("sftp",))
        test("unsigned-query", identity="public-only.pub")
        test("unpaired", identity="unpaired")
        test("wrong-user", user="other")
        test("deny", approvals=1, data=stream)
        test("revoke", approvals=1, commands=1, data=stream)
        test("idle-revoke", approvals=1, commands=1, data=b"key down 4\n")
        test("invalid", approvals=1, commands=1, data=b"key down 4\ninvalid\nmouse move -2 3\n")
        test("oversize", approvals=1, data=b" " * 65 + b"key down 4\n")
        test("unfinished", approvals=1, data=b"key down 4")
        test("exec", channel=("hostname",))
        test("forward")
        test("disconnect", approvals=1, commands=1, data=b"key down 4\n")
        ed25519 = folder / "ed25519"
        ed25519.mkdir()
        for name in ("server", "client"):
            subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "",
                            "-f", str(ed25519 / name)], check=True)
        run_case(ed25519, args.server.resolve(), env, "subsystem", channel=("kvmpad",),
                 ok=True, approvals=1)


if __name__ == "__main__":
    main()
