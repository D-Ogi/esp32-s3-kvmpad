// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

    .section .text.usb_jpeg_equal_blocks,"ax",@progbits
    .align 4
    .global usb_jpeg_equal_blocks
    .type usb_jpeg_equal_blocks,@function
// a2 and a3 are 16-byte aligned. a4 is a positive count of 64-byte groups.
usb_jpeg_equal_blocks:
    entry a1, 64
    addi a6, a1, 16
.Lgroup:
    ee.vld.128.ip q0, a2, 16
    ee.vld.128.ip q1, a3, 16
    ee.xorq q2, q0, q1
    .rept 3
    ee.vld.128.ip q0, a2, 16
    ee.vld.128.ip q1, a3, 16
    ee.xorq q0, q0, q1
    ee.orq q2, q2, q0
    .endr
    ee.vst.128.ip q2, a6, 0
    l32i.n a7, a6, 0
    l32i.n a8, a6, 4
    or a7, a7, a8
    l32i.n a8, a6, 8
    or a7, a7, a8
    l32i.n a8, a6, 12
    or a7, a7, a8
    bnez a7, .Ldifferent
    addi.n a4, a4, -1
    bnez a4, .Lgroup
    movi.n a2, 1
    retw.n
.Ldifferent:
    movi.n a2, 0
    retw.n
    .size usb_jpeg_equal_blocks, .-usb_jpeg_equal_blocks
