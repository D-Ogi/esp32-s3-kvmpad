// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
.section .text.start,"ax"
.global _start
.align 4
_start:
    movi a2, 1
    wsr a2, windowstart
    movi a2, -1
    wsr a2, cpenable
    movi a2, 0x40000000
    wsr a2, vecbase
    movi a2, 0x2a
    wsr a2, atomctl
    movi a2, 0x40000
    wsr a2, ps
    rsync
    movi a1, 0x40200000
    call8 qualify
1:  j 1b


// Preserve the interrupted register window across the call0 arithmetic handler.
.section .kvector,"ax"
j emulate_mul
.section .exception,"ax"
.align 4
registers: .word 0x40520000
test_sp: .word 0x40530000
.global emulate_mul
emulate_mul:
  wsr a0,excsave1
  l32r a0,registers
  .irp n,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    s32i a\n,a0,4*\n
  .endr
  rsr a2,excsave1
  s32i a2,a0,0
  rsr a2,sar
  s32i a2,a0,64
  mov a2,a0
  rsr a3,epc1
  l32r a1,test_sp
  call0 high_mul
  beqz a2,other_op
  rsr a3,epc1
  addi a3,a3,3
  wsr a3,epc1
  l32r a0,registers
  l32i a2,a0,64
  wsr a2,sar
  .irp n,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    l32i a\n,a0,4*\n
  .endr
  l32i a0,a0,0
  rfe
other_op: j other_op
