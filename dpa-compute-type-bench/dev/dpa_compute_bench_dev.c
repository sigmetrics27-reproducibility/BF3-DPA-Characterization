#include <doca_dpa_dev.h>
#include <doca_dpa_dev_sync_event.h>
#include <dpaintrin.h>

// asm loops from libdpabenchasm, loop count in and checksum out
#define ASM_PROTO(name) extern uint64_t name(uint64_t)
ASM_PROTO(bench_lat_u64_add); ASM_PROTO(bench_lat_u64_mul); ASM_PROTO(bench_lat_u64_div);
ASM_PROTO(bench_lat_u64_min); ASM_PROTO(bench_lat_u64_max); ASM_PROTO(bench_lat_u64_xor); ASM_PROTO(bench_lat_u64_and);
ASM_PROTO(bench_lat_u32_add); ASM_PROTO(bench_lat_u32_mul); ASM_PROTO(bench_lat_u32_div);
ASM_PROTO(bench_lat_u32_min); ASM_PROTO(bench_lat_u32_max); ASM_PROTO(bench_lat_u32_xor); ASM_PROTO(bench_lat_u32_and);
ASM_PROTO(bench_lat_u8_add);  ASM_PROTO(bench_lat_u8_mul);  ASM_PROTO(bench_lat_u8_div);
ASM_PROTO(bench_lat_u8_min);  ASM_PROTO(bench_lat_u8_max);  ASM_PROTO(bench_lat_u8_xor);  ASM_PROTO(bench_lat_u8_and);
ASM_PROTO(bench_tpu_u64_add); ASM_PROTO(bench_tpu_u64_mul); ASM_PROTO(bench_tpu_u64_div);
ASM_PROTO(bench_tpu_u64_min); ASM_PROTO(bench_tpu_u64_max); ASM_PROTO(bench_tpu_u64_xor); ASM_PROTO(bench_tpu_u64_and);
ASM_PROTO(bench_tpu_u32_add); ASM_PROTO(bench_tpu_u32_mul); ASM_PROTO(bench_tpu_u32_div);
ASM_PROTO(bench_tpu_u32_min); ASM_PROTO(bench_tpu_u32_max); ASM_PROTO(bench_tpu_u32_xor); ASM_PROTO(bench_tpu_u32_and);
ASM_PROTO(bench_tpu_u8_add);  ASM_PROTO(bench_tpu_u8_mul);  ASM_PROTO(bench_tpu_u8_div);
ASM_PROTO(bench_tpu_u8_min);  ASM_PROTO(bench_tpu_u8_max);  ASM_PROTO(bench_tpu_u8_xor);  ASM_PROTO(bench_tpu_u8_and);
ASM_PROTO(bench_lat_base);
ASM_PROTO(bench_tpu_base);
ASM_PROTO(bench_lat_u64_div_base);
ASM_PROTO(bench_tpu_u64_div_base);
ASM_PROTO(bench_lat_u32_div_base);
ASM_PROTO(bench_tpu_u32_div_base);
ASM_PROTO(bench_lat_u8_div_base);
ASM_PROTO(bench_tpu_u8_div_base);
// float loops from libdpabenchfp call the soft-float helpers
ASM_PROTO(bench_lat_f64_add); ASM_PROTO(bench_lat_f64_mul); ASM_PROTO(bench_lat_f64_div);
ASM_PROTO(bench_lat_f64_min); ASM_PROTO(bench_lat_f64_max);
ASM_PROTO(bench_lat_f32_add); ASM_PROTO(bench_lat_f32_mul); ASM_PROTO(bench_lat_f32_div);
ASM_PROTO(bench_lat_f32_min); ASM_PROTO(bench_lat_f32_max);
ASM_PROTO(bench_tpu_f64_add); ASM_PROTO(bench_tpu_f64_mul); ASM_PROTO(bench_tpu_f64_div);
ASM_PROTO(bench_tpu_f32_add); ASM_PROTO(bench_tpu_f32_mul); ASM_PROTO(bench_tpu_f32_div);
ASM_PROTO(bench_lat_fp_base);
ASM_PROTO(bench_tpu_fp_base);
#undef ASM_PROTO

// time the asm call and return counters plus a checksum
#define KERNEL(kname, afn)                                                              \
    __dpa_global__ void kname(doca_dpa_dev_t dpa_ctx,                                   \
                              doca_dpa_dev_sync_event_t cyc_se,                          \
                              doca_dpa_dev_sync_event_t inst_se,                         \
                              doca_dpa_dev_sync_event_t cksum_se,                        \
                              uint64_t op_iterations)                                   \
    {                                                                                    \
        if (dpa_ctx != 0) {                                                             \
            doca_dpa_dev_device_set(dpa_ctx);                                           \
        }                                                                                \
        const uint64_t c0 = __dpa_thread_cycles();                                      \
        const uint64_t i0 = __dpa_thread_inst_ret();                                    \
        const uint64_t cksum = afn(op_iterations);                                      \
        const uint64_t c1 = __dpa_thread_cycles();                                      \
        const uint64_t i1 = __dpa_thread_inst_ret();                                    \
        doca_dpa_dev_sync_event_update_set(cyc_se, c1 - c0);                            \
        doca_dpa_dev_sync_event_update_set(inst_se, i1 - i0);                           \
        doca_dpa_dev_sync_event_update_set(cksum_se, cksum);                            \
    }

// latency uses one chain with 8 ops per loop
KERNEL(k_lat_u64_add,      bench_lat_u64_add)
KERNEL(k_lat_u64_mul,      bench_lat_u64_mul)
KERNEL(k_lat_u64_div,      bench_lat_u64_div)
KERNEL(k_lat_u64_min,      bench_lat_u64_min)
KERNEL(k_lat_u64_max,      bench_lat_u64_max)
KERNEL(k_lat_u64_xor,      bench_lat_u64_xor)
KERNEL(k_lat_u64_and,      bench_lat_u64_and)
KERNEL(k_lat_u32_add,      bench_lat_u32_add)
KERNEL(k_lat_u32_mul,      bench_lat_u32_mul)
KERNEL(k_lat_u32_div,      bench_lat_u32_div)
KERNEL(k_lat_u32_min,      bench_lat_u32_min)
KERNEL(k_lat_u32_max,      bench_lat_u32_max)
KERNEL(k_lat_u32_xor,      bench_lat_u32_xor)
KERNEL(k_lat_u32_and,      bench_lat_u32_and)
KERNEL(k_lat_u8_add,       bench_lat_u8_add)
KERNEL(k_lat_u8_mul,       bench_lat_u8_mul)
KERNEL(k_lat_u8_div,       bench_lat_u8_div)
KERNEL(k_lat_u8_min,       bench_lat_u8_min)
KERNEL(k_lat_u8_max,       bench_lat_u8_max)
KERNEL(k_lat_u8_xor,       bench_lat_u8_xor)
KERNEL(k_lat_u8_and,       bench_lat_u8_and)

// throughput uses 8 independent chains
KERNEL(k_tpu_u64_add,      bench_tpu_u64_add)
KERNEL(k_tpu_u64_mul,      bench_tpu_u64_mul)
KERNEL(k_tpu_u64_div,      bench_tpu_u64_div)
KERNEL(k_tpu_u64_min,      bench_tpu_u64_min)
KERNEL(k_tpu_u64_max,      bench_tpu_u64_max)
KERNEL(k_tpu_u64_xor,      bench_tpu_u64_xor)
KERNEL(k_tpu_u64_and,      bench_tpu_u64_and)
KERNEL(k_tpu_u32_add,      bench_tpu_u32_add)
KERNEL(k_tpu_u32_mul,      bench_tpu_u32_mul)
KERNEL(k_tpu_u32_div,      bench_tpu_u32_div)
KERNEL(k_tpu_u32_min,      bench_tpu_u32_min)
KERNEL(k_tpu_u32_max,      bench_tpu_u32_max)
KERNEL(k_tpu_u32_xor,      bench_tpu_u32_xor)
KERNEL(k_tpu_u32_and,      bench_tpu_u32_and)
KERNEL(k_tpu_u8_add,       bench_tpu_u8_add)
KERNEL(k_tpu_u8_mul,       bench_tpu_u8_mul)
KERNEL(k_tpu_u8_div,       bench_tpu_u8_div)
KERNEL(k_tpu_u8_min,       bench_tpu_u8_min)
KERNEL(k_tpu_u8_max,       bench_tpu_u8_max)
KERNEL(k_tpu_u8_xor,       bench_tpu_u8_xor)
KERNEL(k_tpu_u8_and,       bench_tpu_u8_and)

// shared baselines and fixed-input div controls
KERNEL(k_lat_base,         bench_lat_base)
KERNEL(k_tpu_base,         bench_tpu_base)
KERNEL(k_lat_u8_div_base,  bench_lat_u8_div_base)
KERNEL(k_tpu_u8_div_base,  bench_tpu_u8_div_base)
KERNEL(k_lat_u32_div_base, bench_lat_u32_div_base)
KERNEL(k_tpu_u32_div_base, bench_tpu_u32_div_base)
KERNEL(k_lat_u64_div_base, bench_lat_u64_div_base)
KERNEL(k_tpu_u64_div_base, bench_tpu_u64_div_base)

// soft-float loops, min/max only have latency tests
KERNEL(k_lat_f64_add, bench_lat_f64_add)
KERNEL(k_lat_f64_mul, bench_lat_f64_mul)
KERNEL(k_lat_f64_div, bench_lat_f64_div)
KERNEL(k_lat_f64_min, bench_lat_f64_min)
KERNEL(k_lat_f64_max, bench_lat_f64_max)
KERNEL(k_lat_f32_add, bench_lat_f32_add)
KERNEL(k_lat_f32_mul, bench_lat_f32_mul)
KERNEL(k_lat_f32_div, bench_lat_f32_div)
KERNEL(k_lat_f32_min, bench_lat_f32_min)
KERNEL(k_lat_f32_max, bench_lat_f32_max)
KERNEL(k_tpu_f64_add, bench_tpu_f64_add)
KERNEL(k_tpu_f64_mul, bench_tpu_f64_mul)
KERNEL(k_tpu_f64_div, bench_tpu_f64_div)
KERNEL(k_tpu_f32_add, bench_tpu_f32_add)
KERNEL(k_tpu_f32_mul, bench_tpu_f32_mul)
KERNEL(k_tpu_f32_div, bench_tpu_f32_div)
KERNEL(k_lat_fp_base, bench_lat_fp_base)
KERNEL(k_tpu_fp_base, bench_tpu_fp_base)
