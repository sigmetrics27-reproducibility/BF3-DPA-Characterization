#include <libflexio-dev/flexio_dev_ver.h>

#define FLEXIO_DEV_VER_USED FLEXIO_DEV_VER(25, 10, 0)

#include <libflexio-dev/flexio_dev.h>
#include <dpaintrin.h>

#include "../host/flexio_mw_copy_common.h"

static inline void copy_with_cpu_pointer(void *dst, const void *src, uint32_t bytes)
{
    volatile uint8_t *dst8 = (volatile uint8_t *)dst;
    const volatile uint8_t *src8 = (const volatile uint8_t *)src;
    uint32_t words = bytes / (uint32_t)sizeof(uint64_t);
    uint32_t tail = bytes - words * (uint32_t)sizeof(uint64_t);

    volatile uint64_t *dst64 = (volatile uint64_t *)(void *)dst8;
    const volatile uint64_t *src64 = (const volatile uint64_t *)(const void *)src8;

    for (uint32_t i = 0; i < words; ++i) {
        dst64[i] = src64[i];
    }

    uint32_t offset = words * (uint32_t)sizeof(uint64_t);
    for (uint32_t i = 0; i < tail; ++i) {
        dst8[offset + i] = src8[offset + i];
    }
}

__dpa_rpc__ uint64_t flexio_mw_copy_run_rpc(uint64_t arg_daddr)
{
    struct mw_copy_rpc_args *arg = (struct mw_copy_rpc_args *)arg_daddr;
    const uint64_t total_iters = (uint64_t)arg->warmup_iters + (uint64_t)arg->measure_iters;
    uint64_t start_cycles = 0;
    uint64_t end_cycles = 0;
    flexio_uintptr_t src_daddr = 0;

    arg->copy_status = MW_COPY_STATUS_OK;
    arg->fail_iter = UINT32_MAX;
    arg->total_cycles = 0;

    if (arg->payload_bytes == 0 || arg->measure_iters == 0) {
        return 0;
    }

    if (flexio_dev_window_config(FLEXIO_DEV_WINDOW_ENTITY_0, (uint16_t)arg->window_id, arg->mkey) !=
        FLEXIO_DEV_STATUS_SUCCESS) {
        arg->copy_status = MW_COPY_STATUS_WINDOW_CONFIG_FAILED;
        return 0;
    }

    if (arg->copy_mode == MW_COPY_MODE_CPU_POINTER) {
        if (flexio_dev_window_ptr_acquire(FLEXIO_DEV_WINDOW_ENTITY_0, arg->src_haddr, &src_daddr) !=
            FLEXIO_DEV_STATUS_SUCCESS) {
            arg->copy_status = MW_COPY_STATUS_PTR_ACQUIRE_FAILED;
            return 0;
        }
    } else if (arg->copy_mode != MW_COPY_MODE_WINDOW_API) {
        arg->copy_status = MW_COPY_STATUS_BAD_MODE;
        return 0;
    }

    for (uint64_t iter = 0; iter < total_iters; ++iter) {
        if (iter == (uint64_t)arg->warmup_iters) {
            start_cycles = __dpa_thread_cycles();
        }

        if (arg->copy_mode == MW_COPY_MODE_WINDOW_API) {
            if (arg->copy_direction == MW_COPY_DIRECTION_H2D) {
                if (flexio_dev_window_copy_from_host(FLEXIO_DEV_WINDOW_ENTITY_0,
                                                     (void *)(uintptr_t)arg->dst_daddr,
                                                     arg->src_haddr,
                                                     arg->payload_bytes) != FLEXIO_DEV_STATUS_SUCCESS) {
                    arg->copy_status = MW_COPY_STATUS_COPY_FAILED;
                    arg->fail_iter = (uint32_t)iter;
                    return 0;
                }
            } else if (arg->copy_direction == MW_COPY_DIRECTION_D2H) {
                if (flexio_dev_window_copy_to_host(FLEXIO_DEV_WINDOW_ENTITY_0,
                                                   arg->src_haddr,
                                                   (const void *)(uintptr_t)arg->dst_daddr,
                                                   arg->payload_bytes) != FLEXIO_DEV_STATUS_SUCCESS) {
                    arg->copy_status = MW_COPY_STATUS_COPY_FAILED;
                    arg->fail_iter = (uint32_t)iter;
                    return 0;
                }
            } else {
                arg->copy_status = MW_COPY_STATUS_BAD_DIRECTION;
                arg->fail_iter = (uint32_t)iter;
                return 0;
            }
        } else {
            if (arg->copy_direction == MW_COPY_DIRECTION_H2D) {
                __dpa_thread_window_read_inv();
                copy_with_cpu_pointer((void *)(uintptr_t)arg->dst_daddr,
                                      (const void *)(uintptr_t)src_daddr,
                                      arg->payload_bytes);
                __dpa_thread_memory_writeback();
            } else if (arg->copy_direction == MW_COPY_DIRECTION_D2H) {
                copy_with_cpu_pointer((void *)(uintptr_t)src_daddr,
                                      (const void *)(uintptr_t)arg->dst_daddr,
                                      arg->payload_bytes);
                __dpa_thread_window_writeback();
            } else {
                arg->copy_status = MW_COPY_STATUS_BAD_DIRECTION;
                arg->fail_iter = (uint32_t)iter;
                return 0;
            }
        }
    }

    end_cycles = __dpa_thread_cycles();
    if (end_cycles >= start_cycles) {
        arg->total_cycles = end_cycles - start_cycles;
    } else {
        arg->total_cycles = ((1ULL << 32) - 1ULL) - start_cycles + end_cycles;
    }

    return arg->total_cycles;
}
