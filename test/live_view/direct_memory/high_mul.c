// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
// The generic de233_fpu lacks MULUH/MULSH. Compute the high half with four 16-bit products.
#include <stdint.h>
volatile unsigned emulated[3];
int high_mul(uint32_t *reg, const uint8_t *p) {
  unsigned code = p[0] | (p[1] << 8) | (p[2] << 16);
  unsigned op = code >> 16, dst = (code >> 12) & 15;
  unsigned a = reg[(code >> 8) & 15], b = reg[(code >> 4) & 15];
  if ((code & 15) != 0 || (op != 0xa2 && op != 0xb2)) {
    emulated[2] = code;
    return 0;
  }
  unsigned p0 = (a & 65535u) * (b & 65535u);
  unsigned t = (a >> 16) * (b & 65535u) + (p0 >> 16);
  unsigned u = (a & 65535u) * (b >> 16) + (t & 65535u);
  unsigned hi = (a >> 16) * (b >> 16) + (t >> 16) + (u >> 16);
  if (op == 0xb2) {
    if (a >> 31)
      hi -= b;
    if (b >> 31)
      hi -= a;
  }
  reg[dst] = hi;
  ++emulated[op == 0xb2];
  return 1;
}
