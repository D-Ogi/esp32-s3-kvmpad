# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Reproduce the portable verification subset from an unpacked aws-lc-sys 0.41.0 crate."""
import argparse
import hashlib
from pathlib import Path
import re
import shutil

PINS = {
    "crypto/fipsmodule/curve25519/curve25519_nohw.c": "8b2a989419e0dc810bf9fb2bdba1db0599e20f826a6be2e87a99c83454c1d86e",
    "crypto/fipsmodule/curve25519/internal.h": "2f3b9765663b76585b36a7a85fd022ff77aef819317be1fa130b0ecbc66fa36a",
    "crypto/fipsmodule/curve25519/curve25519_tables.h": "3c3e5be8fb0ed8fe43890fdf1659af42c580da69b9de45b51a780a99bcd671ca",
    "third_party/fiat/curve25519_32.h": "b5260ae29ecd25969588fa157b02173d98e21b63739bea24392acc92c890dafc",
}

PORT = """// Verification-only, portable 32-bit subset from aws-lc-sys 0.41.0.
// Upstream field functions and group constants retain their published values.
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "curve25519_32.h"
#define declassify_assert(x) assert(x)
#define OPENSSL_memcpy memcpy
#define OPENSSL_memmove memmove
#define OPENSSL_memset memset
#define CRYPTO_memcmp memcmp
#define OPENSSL_STATIC_ASSERT(x, name) _Static_assert(x, #name);
"""


def reproduce(crate, output):
    aws = crate / "aws-lc"
    for path, digest in PINS.items():
        if hashlib.sha256((aws / path).read_bytes()).hexdigest() != digest:
            raise ValueError(f"unexpected upstream file {path}")
    src = aws / "crypto/fipsmodule/curve25519"
    curve = (src / "curve25519_nohw.c").read_text(encoding="utf8")
    types = (src / "internal.h").read_text(encoding="utf8")
    tables = (src / "curve25519_tables.h").read_text(encoding="utf8")
    lines = curve.splitlines(keepends=True)
    parts = ["".join(lines[:9]), PORT,
             types[types.index("typedef struct fe { uint32_t"):types.index("#endif\n\n// ge means")],
             types[types.index("// ge means"):types.index("void x25519_ge_tobytes")],
             "".join(lines[91:130]), "".join(lines[132:134]),
             tables[:tables.index("#if defined(OPENSSL_SMALL)")], tables[tables.index("static const ge_precomp k25519Bi"):]]
    declarations = re.compile(r"^(?:static (?:inline )?)?(?:void|int|uint64_t|uint8_t|int64_t) "
                              r"(\w+)\([^;{]+\)\s*\{[\s\S]*?^}", re.MULTILINE)
    functions = {m[1]: m[0] for m in declarations.finditer(curve)}
    needed = {"x25519_ge_frombytes_vartime", "ge_p3_tobytes", "x25519_ge_tobytes", "x25519_sc_reduce",
              "fe_neg", "fe_carry", "ge_double_scalarmult_vartime"}
    while True:
        dependencies = {name for fn in needed for name in re.findall(r"\b(\w+)\s*\(", functions[fn]) if name in functions}
        if dependencies <= needed:
            break
        needed.update(dependencies)
    for name, body in functions.items():
        if name in needed:
            parts.extend(("" if body.startswith("static ") else "static ", body, "\n\n"))
    output.mkdir(parents=True, exist_ok=True)
    (output / "edwards_verify.inc").write_text("".join(parts), encoding="utf8", newline="\n")
    shutil.copyfile(aws / "third_party/fiat/curve25519_32.h", output / "curve25519_32.h")
    shutil.copyfile(aws / "third_party/fiat/LICENSE", output / "LICENSE.fiat")
    shutil.copyfile(aws / "LICENSE", output / "LICENSE")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("crate", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    reproduce(args.crate, args.output)
