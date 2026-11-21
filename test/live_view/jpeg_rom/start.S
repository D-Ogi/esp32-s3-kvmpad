// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
.section .text.start,"ax"
.global _start
.align 4
_start:
    movi a2, 1
    wsr a2, windowstart
    movi a2, 0x40000000
    wsr a2, vecbase
    movi a2, 0x40000
    wsr a2, ps
    rsync
#ifdef USB_FIT_TEST
    movi a1, 0x40c00000
#else
    movi a1, 0x40200000
#endif
    call8 qualify
1:  j 1b
