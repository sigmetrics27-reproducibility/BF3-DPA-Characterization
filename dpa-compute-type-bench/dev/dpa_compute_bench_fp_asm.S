// time soft-float helpers with 8 chained calls or 8 independent lanes
// a0 takes the loop count and returns a checksum
// keep values away from zero and infinity, min/max include compare and select
// matching baselines give an estimate of the call cost

    .text

# save the same registers in every float loop and baseline
.macro FP_PROLOGUE
    addi  sp, sp, -96
    sd    ra, 88(sp)
    sd    s0, 80(sp)
    sd    s1, 72(sp)
    sd    s2, 64(sp)
    sd    s3, 56(sp)
    sd    s4, 48(sp)
    sd    s5, 40(sp)
    sd    s6, 32(sp)
    sd    s7, 24(sp)
    sd    s8, 16(sp)
    sd    s9, 8(sp)
    sd    s10, 0(sp)
.endm
.macro FP_EPILOGUE
    ld    ra, 88(sp)
    ld    s0, 80(sp)
    ld    s1, 72(sp)
    ld    s2, 64(sp)
    ld    s3, 56(sp)
    ld    s4, 48(sp)
    ld    s5, 40(sp)
    ld    s6, 32(sp)
    ld    s7, 24(sp)
    ld    s8, 16(sp)
    ld    s9, 8(sp)
    ld    s10, 0(sp)
    addi  sp, sp, 96
    ret
.endm

# latency, 8 dependent helper calls
.macro FP_LAT1 name, FN, SEED, OP
    .p2align 6
    .globl \name
    .type  \name,@function
\name:
    FP_PROLOGUE
    mv    s0, a0
    li    s1, \SEED
    li    s2, \OP
1:
.rept 8
    mv    a0, s1
    mv    a1, s2
    call  \FN
    mv    s1, a0
.endr
    addi  s0, s0, -1
    bnez  s0, 1b
    mv    a0, s1
    FP_EPILOGUE
.endm

# min/max latency, compare then select
# use bltz for min and bgtz for max
.macro FP_LAT_CMP name, FN, SEED, OP, BR
    .p2align 6
    .globl \name
    .type  \name,@function
\name:
    FP_PROLOGUE
    mv    s0, a0
    li    s1, \SEED
    li    s2, \OP
1:
.rept 8
    mv    a0, s1
    mv    a1, s2
    call  \FN
    \BR   a0, 7f
    mv    s1, s2
7:
.endr
    addi  s0, s0, -1
    bnez  s0, 1b
    mv    a0, s1
    FP_EPILOGUE
.endm

# throughput, 8 independent chains
.macro FP_TPU1 name, FN, SEED, OP
    .p2align 6
    .globl \name
    .type  \name,@function
\name:
    FP_PROLOGUE
    li    s1, \SEED
    li    s2, \SEED
    li    s3, \SEED
    li    s4, \SEED
    li    s5, \SEED
    li    s6, \SEED
    li    s7, \SEED
    li    s8, \SEED
    li    s9, \OP
    mv    s10, a0
1:
    mv    a0, s1; mv a1, s9; call \FN; mv s1, a0
    mv    a0, s2; mv a1, s9; call \FN; mv s2, a0
    mv    a0, s3; mv a1, s9; call \FN; mv s3, a0
    mv    a0, s4; mv a1, s9; call \FN; mv s4, a0
    mv    a0, s5; mv a1, s9; call \FN; mv s5, a0
    mv    a0, s6; mv a1, s9; call \FN; mv s6, a0
    mv    a0, s7; mv a1, s9; call \FN; mv s7, a0
    mv    a0, s8; mv a1, s9; call \FN; mv s8, a0
    addi  s10, s10, -1
    bnez  s10, 1b
    xor   a0, s1, s2
    xor   a0, a0, s3
    xor   a0, a0, s4
    xor   a0, a0, s5
    xor   a0, a0, s6
    xor   a0, a0, s7
    xor   a0, a0, s8
    FP_EPILOGUE
.endm

# same setup and loop, without the soft-float calls
    .p2align 6
    .globl bench_lat_fp_base
    .type  bench_lat_fp_base,@function
bench_lat_fp_base:
    FP_PROLOGUE
    mv    s0, a0
    li    s1, 0x3ff0000010000000
    li    s2, 0x3ff0000000000000
1:
    addi  s0, s0, -1
    bnez  s0, 1b
    mv    a0, s1
    FP_EPILOGUE

    .p2align 6
    .globl bench_tpu_fp_base
    .type  bench_tpu_fp_base,@function
bench_tpu_fp_base:
    FP_PROLOGUE
    li    s1, 0x3ff0000010000000
    li    s2, 0x3ff0000010000000
    li    s3, 0x3ff0000010000000
    li    s4, 0x3ff0000010000000
    li    s5, 0x3ff0000010000000
    li    s6, 0x3ff0000010000000
    li    s7, 0x3ff0000010000000
    li    s8, 0x3ff0000010000000
    li    s9, 0x3ff0000000000000
    mv    s10, a0
1:
    addi  s10, s10, -1
    bnez  s10, 1b
    xor   a0, s1, s2
    xor   a0, a0, s3
    xor   a0, a0, s4
    xor   a0, a0, s5
    xor   a0, a0, s6
    xor   a0, a0, s7
    xor   a0, a0, s8
    FP_EPILOGUE

# f64
FP_LAT1     bench_lat_f64_add, __adddf3, 0x3ff0000010000000, 0x3ff0000000000000
FP_LAT1     bench_lat_f64_mul, __muldf3, 0x3ff0000010000000, 0x3ff0000010000000
FP_LAT1     bench_lat_f64_div, __divdf3, 0x3ff0000010000000, 0x3ff0000010000000
FP_LAT_CMP  bench_lat_f64_min, __ltdf2,  0x3ff0000010000000, 0x3ff0000000000000, bltz
FP_LAT_CMP  bench_lat_f64_max, __gtdf2,  0x3ff0000010000000, 0x3ff0000000000000, bgtz

FP_TPU1     bench_tpu_f64_add, __adddf3, 0x3ff0000010000000, 0x3ff0000000000000
FP_TPU1     bench_tpu_f64_mul, __muldf3, 0x3ff0000010000000, 0x3ff0000010000000
FP_TPU1     bench_tpu_f64_div, __divdf3, 0x3ff0000010000000, 0x3ff0000010000000

# f32
FP_LAT1     bench_lat_f32_add, __addsf3, 0x3f800008, 0x3f800000
FP_LAT1     bench_lat_f32_mul, __mulsf3, 0x3f800008, 0x3f800008
FP_LAT1     bench_lat_f32_div, __divsf3, 0x3f800008, 0x3f800008
FP_LAT_CMP  bench_lat_f32_min, __ltsf2,  0x3f800008, 0x3f800000, bltz
FP_LAT_CMP  bench_lat_f32_max, __gtsf2,  0x3f800008, 0x3f800000, bgtz

FP_TPU1     bench_tpu_f32_add, __addsf3, 0x3f800008, 0x3f800000
FP_TPU1     bench_tpu_f32_mul, __mulsf3, 0x3f800008, 0x3f800008
FP_TPU1     bench_tpu_f32_div, __divsf3, 0x3f800008, 0x3f800008
