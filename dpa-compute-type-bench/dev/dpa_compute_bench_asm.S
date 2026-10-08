// integer hot loops, a0 takes the loop count and returns a checksum
// latency uses 8 chained ops, throughput uses 8 independent lanes
// div keeps its input fixed and has matching controls
// baseline subtraction gives an estimate of the op cost

    .text

# latency, 8 ops in one chain
.macro LAT_FN name, op, mask=""
    .p2align 6
    .globl \name
    .type  \name,@function
\name:
    mv    t2, a0
    li    t0, 0x243f6a8885a308d3
    li    t1, 3                       # shared operand b
1:
.rept 8
    \op   t0, t0, t1
.ifnb \mask
    \mask t0, t0, 0xff
.endif
.endr
    addi  t2, t2, -1
    bnez  t2, 1b
    mv    a0, t0                      # checksum = final acc
    ret
.endm

# throughput, 8 independent chains
.macro TPU_FN name, op, mask=""
    .p2align 6
    .globl \name
    .type  \name,@function
\name:
    mv    t2, a0
    li    t1, 3                       # shared operand b
    li    a1, 0x21                    # one seed per lane
    li    a2, 0x22
    li    a3, 0x23
    li    a4, 0x24
    li    a5, 0x25
    li    a6, 0x26
    li    a7, 0x27
    li    t0, 0x28
1:
    \op   a1, a1, t1
    \op   a2, a2, t1
    \op   a3, a3, t1
    \op   a4, a4, t1
    \op   a5, a5, t1
    \op   a6, a6, t1
    \op   a7, a7, t1
    \op   t0, t0, t1
.ifnb \mask
    \mask a1, a1, 0xff
    \mask a2, a2, 0xff
    \mask a3, a3, 0xff
    \mask a4, a4, 0xff
    \mask a5, a5, 0xff
    \mask a6, a6, 0xff
    \mask a7, a7, 0xff
    \mask t0, t0, 0xff
.endif
    addi  t2, t2, -1
    bnez  t2, 1b
    xor   a0, a1, a2                  # mix the lanes and operand into a checksum
    xor   a0, a0, a3
    xor   a0, a0, a4
    xor   a0, a0, a5
    xor   a0, a0, a6
    xor   a0, a0, a7
    xor   a0, a0, t0
    xor   a0, a0, t1
    ret
.endm

# same setup and loop, without the measured ops

    .p2align 6
    .globl bench_lat_base
    .type  bench_lat_base,@function
bench_lat_base:
    mv    t2, a0
    li    t0, 0x243f6a8885a308d3
    li    t1, 3
1:
    addi  t2, t2, -1
    bnez  t2, 1b
    mv    a0, t0
    ret

    .p2align 6
    .globl bench_tpu_base
    .type  bench_tpu_base,@function
bench_tpu_base:
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
1:
    addi  t2, t2, -1
    bnez  t2, 1b
    xor   a0, a1, a2
    xor   a0, a0, a3
    xor   a0, a0, a4
    xor   a0, a0, a5
    xor   a0, a0, a6
    xor   a0, a0, a7
    xor   a0, a0, t0
    xor   a0, a0, t1
    ret

# fixed-input division
# xor restores the dividend after each div and keeps the dependency
# the xor-only control gives a baseline estimate
.macro LAT_DIV_FN name, op, dividend, restore, with_div, mask=""
    .p2align 6
    .globl \name
    .type  \name,@function
\name:
    mv    t2, a0
    li    t0, \dividend
    li    t1, 3
    li    t3, \restore
1:
.rept 8
.if \with_div
    \op   t0, t0, t1
.endif
.ifnb \mask
    \mask t0, t0, 0xff
.endif
    xor   t0, t0, t3
.endr
    addi  t2, t2, -1
    bnez  t2, 1b
    mv    a0, t0
    ret
.endm

LAT_DIV_FN bench_lat_u8_div, divuw, 0xff, 0xaa, 1, andi
LAT_DIV_FN bench_lat_u8_div_base, divuw, 0xff, 0xaa, 0, andi
LAT_DIV_FN bench_lat_u32_div, divuw, 0xffffffff, 0xaaaaaaaa, 1
LAT_DIV_FN bench_lat_u32_div_base, divuw, 0xffffffff, 0xaaaaaaaa, 0
LAT_DIV_FN bench_lat_u64_div, divu, 0x7fffffffffffffff, 0x5555555555555555, 1
LAT_DIV_FN bench_lat_u64_div_base, divu, 0x7fffffffffffffff, 0x5555555555555555, 0

# 8 divisions with fixed input, use the same setup and checksum in the control
.macro TPU_DIV_FN name, op, dividend, quotient, with_div, mask=""
    .p2align 6
    .globl \name
    .type  \name,@function
\name:
    mv    t2, a0
    li    t1, 3
    li    t5, \dividend
    li    a1, \quotient
    mv    a2, a1
    mv    a3, a1
    mv    a4, a1
    mv    a5, a1
    mv    a6, a1
    mv    a7, a1
    mv    t0, a1
1:
.if \with_div
    \op   a1, t5, t1
    \op   a2, t5, t1
    \op   a3, t5, t1
    \op   a4, t5, t1
    \op   a5, t5, t1
    \op   a6, t5, t1
    \op   a7, t5, t1
    \op   t0, t5, t1
.endif
.ifnb \mask
    \mask a1, a1, 0xff
    \mask a2, a2, 0xff
    \mask a3, a3, 0xff
    \mask a4, a4, 0xff
    \mask a5, a5, 0xff
    \mask a6, a6, 0xff
    \mask a7, a7, 0xff
    \mask t0, t0, 0xff
.endif
    addi  t2, t2, -1
    bnez  t2, 1b
    add   a0, a1, a2
    add   a0, a0, a3
    add   a0, a0, a4
    add   a0, a0, a5
    add   a0, a0, a6
    add   a0, a0, a7
    add   a0, a0, t0
    add   a0, a0, t1
    ret
.endm

TPU_DIV_FN bench_tpu_u8_div, divuw, 0xff, 0x55, 1, andi
TPU_DIV_FN bench_tpu_u8_div_base, divuw, 0xff, 0x55, 0, andi
TPU_DIV_FN bench_tpu_u32_div, divuw, 0xffffffff, 0x55555555, 1
TPU_DIV_FN bench_tpu_u32_div_base, divuw, 0xffffffff, 0x55555555, 0
TPU_DIV_FN bench_tpu_u64_div, divu, 0x7fffffffffffffff, 0x2aaaaaaaaaaaaaaa, 1
TPU_DIV_FN bench_tpu_u64_div_base, divu, 0x7fffffffffffffff, 0x2aaaaaaaaaaaaaaa, 0

# latency workloads
# u64
LAT_FN bench_lat_u64_add, add
LAT_FN bench_lat_u64_mul, mul
LAT_FN bench_lat_u64_min, minu
LAT_FN bench_lat_u64_max, maxu
LAT_FN bench_lat_u64_xor, xor
LAT_FN bench_lat_u64_and, and
# u32
LAT_FN bench_lat_u32_add, addw
LAT_FN bench_lat_u32_mul, mulw
LAT_FN bench_lat_u32_min, minu
LAT_FN bench_lat_u32_max, maxu
LAT_FN bench_lat_u32_xor, xor
LAT_FN bench_lat_u32_and, and
# u8, mask add/mul/div back to 8 bits
LAT_FN bench_lat_u8_add,  addw, andi
LAT_FN bench_lat_u8_mul,  mulw, andi
LAT_FN bench_lat_u8_min,  minu
LAT_FN bench_lat_u8_max,  maxu
LAT_FN bench_lat_u8_xor,  xor
LAT_FN bench_lat_u8_and,  and

# throughput workloads
# u64
TPU_FN bench_tpu_u64_add, add
TPU_FN bench_tpu_u64_mul, mul
TPU_FN bench_tpu_u64_min, minu
TPU_FN bench_tpu_u64_max, maxu
TPU_FN bench_tpu_u64_xor, xor
TPU_FN bench_tpu_u64_and, and
# u32
TPU_FN bench_tpu_u32_add, addw
TPU_FN bench_tpu_u32_mul, mulw
TPU_FN bench_tpu_u32_min, minu
TPU_FN bench_tpu_u32_max, maxu
TPU_FN bench_tpu_u32_xor, xor
TPU_FN bench_tpu_u32_and, and
# u8
TPU_FN bench_tpu_u8_add,  addw,  andi
TPU_FN bench_tpu_u8_mul,  mulw,  andi
TPU_FN bench_tpu_u8_min,  minu
TPU_FN bench_tpu_u8_max,  maxu
TPU_FN bench_tpu_u8_xor,  xor
TPU_FN bench_tpu_u8_and,  and
