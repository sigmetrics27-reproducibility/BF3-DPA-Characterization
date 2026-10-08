// concurrent ALU loops, loop count in and checksum out
// 10 lanes repeated 64 times gives 640 ops per outer loop
// timing includes loop overhead, no baseline subtraction

    .text

#define LANES   10
#define REPEATS 64

.macro SC_ALU_FN name, op, mask=""
    .p2align 6
    .globl \name
    .type  \name,@function
\name:
    mv    t2, a0
    li    t1, 3
    li    a1, 0x21
    li    a2, 0x22
    li    a3, 0x23
    li    a4, 0x24
    li    a5, 0x25
    li    a6, 0x26
    li    a7, 0x27
    li    t0, 0x28
    li    t3, 0x29
    li    t4, 0x2a
1:
.rept REPEATS
    \op   a1, a1, t1
    \op   a2, a2, t1
    \op   a3, a3, t1
    \op   a4, a4, t1
    \op   a5, a5, t1
    \op   a6, a6, t1
    \op   a7, a7, t1
    \op   t0, t0, t1
    \op   t3, t3, t1
    \op   t4, t4, t1
.ifnb \mask
    \mask a1, a1, 0xff
    \mask a2, a2, 0xff
    \mask a3, a3, 0xff
    \mask a4, a4, 0xff
    \mask a5, a5, 0xff
    \mask a6, a6, 0xff
    \mask a7, a7, 0xff
    \mask t0, t0, 0xff
    \mask t3, t3, 0xff
    \mask t4, t4, 0xff
.endif
.endr
    addi  t2, t2, -1
    bnez  t2, 1b
    xor   a0, a1, a2
    xor   a0, a0, a3
    xor   a0, a0, a4
    xor   a0, a0, a5
    xor   a0, a0, a6
    xor   a0, a0, a7
    xor   a0, a0, t0
    xor   a0, a0, t3
    xor   a0, a0, t4
    xor   a0, a0, t1
    ret
.endm

SC_ALU_FN bench_sc_u64_add, add
SC_ALU_FN bench_sc_u64_mul, mul
SC_ALU_FN bench_sc_u64_xor, xor
SC_ALU_FN bench_sc_u64_and, and
SC_ALU_FN bench_sc_u64_min, minu
SC_ALU_FN bench_sc_u64_max, maxu

# fixed-input division; the u64 worker is also used by the multicore test
.macro SC_DIV_FN name, op, dividend, mask=""
    .p2align 6
    .globl \name
    .type  \name,@function
\name:
    mv    t2, a0
    li    t1, 3
    li    t5, \dividend
1:
.rept REPEATS
    \op   a1, t5, t1
    \op   a2, t5, t1
    \op   a3, t5, t1
    \op   a4, t5, t1
    \op   a5, t5, t1
    \op   a6, t5, t1
    \op   a7, t5, t1
    \op   t0, t5, t1
    \op   t3, t5, t1
    \op   t4, t5, t1
.ifnb \mask
    \mask a1, a1, 0xff
    \mask a2, a2, 0xff
    \mask a3, a3, 0xff
    \mask a4, a4, 0xff
    \mask a5, a5, 0xff
    \mask a6, a6, 0xff
    \mask a7, a7, 0xff
    \mask t0, t0, 0xff
    \mask t3, t3, 0xff
    \mask t4, t4, 0xff
.endif
.endr
    addi  t2, t2, -1
    bnez  t2, 1b
    add   a0, a1, a2
    add   a0, a0, a3
    add   a0, a0, a4
    add   a0, a0, a5
    add   a0, a0, a6
    add   a0, a0, a7
    add   a0, a0, t0
    add   a0, a0, t3
    add   a0, a0, t4
    add   a0, a0, t1
    ret
.endm

SC_DIV_FN bench_sc_u8_div, divuw, 0xff, andi
SC_DIV_FN bench_sc_u32_div, divuw, 0xffffffff
SC_DIV_FN bench_sc_u64_div, divu, 0x7fffffffffffffff

SC_ALU_FN bench_sc_u32_add, addw
SC_ALU_FN bench_sc_u32_mul, mulw
SC_ALU_FN bench_sc_u32_xor, xor
SC_ALU_FN bench_sc_u32_and, and
SC_ALU_FN bench_sc_u32_min, minu
SC_ALU_FN bench_sc_u32_max, maxu

SC_ALU_FN bench_sc_u8_add, addw, andi
SC_ALU_FN bench_sc_u8_mul, mulw, andi
SC_ALU_FN bench_sc_u8_xor, xor
SC_ALU_FN bench_sc_u8_and, and
SC_ALU_FN bench_sc_u8_min, minu
SC_ALU_FN bench_sc_u8_max, maxu
