#include <doca_dpa_dev.h>
#include <doca_dpa_dev_buf.h>
#include <dpaintrin.h>

#include "../host/dpa_external_ptr_copy_common.h"

static __always_inline void copy_bytes(volatile uint8_t *dst, const volatile uint8_t *src, uint32_t bytes)
{
    uint32_t words = bytes / (uint32_t)sizeof(uint64_t);
    uint32_t tail = bytes - words * (uint32_t)sizeof(uint64_t);

    volatile uint64_t *dst64 = (volatile uint64_t *)(void *)dst;
    const volatile uint64_t *src64 = (const volatile uint64_t *)(const void *)src;

    for (uint32_t i = 0; i < words; ++i) {
        dst64[i] = src64[i];
    }

    uint32_t offset = words * (uint32_t)sizeof(uint64_t);
    for (uint32_t i = 0; i < tail; ++i) {
        dst[offset + i] = src[offset + i];
    }
}

__dpa_rpc__ uint64_t dpa_external_ptr_copy_run_rpc(doca_dpa_dev_t dpa_ctx_handle,
                                                   doca_dpa_dev_mmap_t host_mmap_handle,
                                                   uint64_t host_src_addr,
                                                   uint64_t host_dst_addr,
                                                   doca_dpa_dev_uintptr_t local_dpa_buf_addr,
                                                   uint32_t mode,
                                                   uint32_t warmup_iters,
                                                   uint32_t measure_iters,
                                                   uint32_t payload_bytes)
{
    const uint64_t total_iters = (uint64_t)warmup_iters + (uint64_t)measure_iters;
    uint64_t start_cycles = 0;
    uint64_t end_cycles = 0;

    if (measure_iters == 0 || payload_bytes == 0) {
        return 0;
    }

    if (dpa_ctx_handle != 0) {
        doca_dpa_dev_device_set(dpa_ctx_handle);
    }

    doca_dpa_dev_uintptr_t host_src_ptr =
        doca_dpa_dev_mmap_get_external_ptr(host_mmap_handle, host_src_addr);
    doca_dpa_dev_uintptr_t host_dst_ptr =
        doca_dpa_dev_mmap_get_external_ptr(host_mmap_handle, host_dst_addr);

    volatile uint8_t *local_ptr = (volatile uint8_t *)(uintptr_t)local_dpa_buf_addr;
    const volatile uint8_t *host_src = (const volatile uint8_t *)(uintptr_t)host_src_ptr;
    volatile uint8_t *host_dst = (volatile uint8_t *)(uintptr_t)host_dst_ptr;

    for (uint64_t iter = 0; iter < total_iters; ++iter) {
        if (iter == (uint64_t)warmup_iters) {
            start_cycles = __dpa_thread_cycles();
        }

        if (mode == DPA_EXTERNAL_PTR_COPY_MODE_H2D) {
            __dpa_thread_window_read_inv(); // invalidate the host window before reading
            copy_bytes(local_ptr, host_src, payload_bytes);
            __dpa_thread_memory_writeback(); // write back for other threads to see
        } else if (mode == DPA_EXTERNAL_PTR_COPY_MODE_D2H) {
            copy_bytes(host_dst, local_ptr, payload_bytes);
            __dpa_thread_window_writeback();
        } else {
            return 0;
        }
    }

    end_cycles = __dpa_thread_cycles();

    if (end_cycles >= start_cycles) {
        return end_cycles - start_cycles;
    }

    return ((1ULL << 32) - 1ULL) - start_cycles + end_cycles;
}
