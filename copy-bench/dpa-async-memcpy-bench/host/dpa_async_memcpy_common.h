#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum dpa_async_memcpy_mode {
    DPA_ASYNC_MEMCPY_MODE_H2D = 0,
    DPA_ASYNC_MEMCPY_MODE_D2H = 1,
};

enum dpa_async_memcpy_status {
    DPA_ASYNC_MEMCPY_STATUS_OK = 0,
    DPA_ASYNC_MEMCPY_STATUS_BAD_MODE = 1,
    DPA_ASYNC_MEMCPY_STATUS_BAD_ARGS = 2,
};

struct dpa_async_memcpy_thread_cmd {
    uint64_t seq;
    uint64_t cycles;
    uint32_t status;
    uint32_t mode;
    uint32_t warmup_iters;
    uint32_t measure_iters;
    uint32_t payload_bytes;
    uint32_t reserved;

    uint64_t dpa_ctx_handle;
    uint64_t async_ops_handle;
    uint64_t completion_handle;
    uint64_t completion_se;

    uint32_t host_mmap_handle;
    uint32_t local_mmap_handle;
    uint64_t host_src_addr;
    uint64_t host_dst_addr;
    uint64_t local_addr;
};

#ifdef __cplusplus
}
#endif
