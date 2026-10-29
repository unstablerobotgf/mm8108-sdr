# UPHY streaming packer: capture ring (16-bit I/Q words) -> packed ring (top 8 bits, 2 samples/word, bytes I0 I1 Q0 Q1).
# Runs forever on UPHY (entered from the ring-take ASSERT). Publishes the packed write address at CTRL (like 0x40906c)
# and a heartbeat at CTRL+4. Everything lives in UPHY DMEM: cross-core loads/stores cost ~40 cycles each.
    .equ PTR,      0x40906c          # capture write pointer (byte address)
    .equ CTRL,     0x510f00
    # IN_BASE/IN_END/OUT_BASE/OUT_END come from --defsym (must match mm8108.cpp PK_*)
    .text
    .globl _start
_start:
    li   s0, IN_BASE
    li   s1, IN_END
    li   s2, OUT_BASE
    li   s3, OUT_END
    li   s4, 0x00ff00ff
    li   s8, CTRL
    li   s9, PTR
    li   s5, 0                       # read address; 0 until the capture ring is live
    mv   s6, s2                      # packed write address
    li   s10, 0                      # heartbeat
    sw   s6, 0(s8)
loop:
    addi s10, s10, 1
    sw   s10, 4(s8)
    lw   t0, 0(s9)
    bltu t0, s0, loop                # pointer outside our ring: not set up yet (or a glitch)
    bgeu t0, s1, loop
    andi t0, t0, -8                  # whole pairs only
    bnez s5, live
    mv   s5, t0                      # first valid pointer: start from here
    j    loop
live:
    bne  t0, s5, work
    li   t3, IDLE_SPIN               # nothing new: back off locally instead of hammering the peripheral bus
3:  addi t3, t3, -1
    bnez t3, 3b
    j    loop
work:
    bgeu t0, s5, ahead
    sub  t1, s1, s5                  # writer wrapped: drain to the end of the ring first
    j    have
ahead:
    sub  t1, t0, s5
have:
    srli t1, t1, 3                   # pairs available
    sub  t2, s3, s6
    srli t2, t2, 2                   # output words before the packed ring wraps
    bgeu t2, t1, pack
    mv   t1, t2
pack:
    lw   a0, 0(s5)
    lw   a1, 4(s5)
    srli a0, a0, 8
    srli a1, a1, 8
    and  a0, a0, s4
    and  a1, a1, s4
    slli a1, a1, 8
    or   a0, a0, a1
    sw   a0, 0(s6)
    addi s5, s5, 8
    addi s6, s6, 4
    addi t1, t1, -1
    bnez t1, pack
    bne  s5, s1, 1f
    mv   s5, s0
1:  bne  s6, s3, 2f
    mv   s6, s2
2:  sw   s6, 0(s8)
    j    loop
