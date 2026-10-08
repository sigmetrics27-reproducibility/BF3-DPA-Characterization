#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mw_copy_rpc_args {
    uint32_t window_id;
    uint32_t mkey;
    uint32_t copy_mode;
    uint32_t copy_direction;
    uint32_t payload_bytes;
    uint32_t warmup_iters;
    uint32_t measure_iters;
    uint32_t copy_status;
    uint32_t fail_iter;
    uint32_t reserved0;
    uint64_t src_haddr;
    uint64_t dst_daddr;
    uint64_t total_cycles;
};

enum mw_copy_mode {
    MW_COPY_MODE_WINDOW_API = 0,
    MW_COPY_MODE_CPU_POINTER = 1,
};

enum mw_copy_direction {
    MW_COPY_DIRECTION_H2D = 0,
    MW_COPY_DIRECTION_D2H = 1,
};

enum mw_copy_status {
    MW_COPY_STATUS_OK = 0,
    MW_COPY_STATUS_WINDOW_CONFIG_FAILED = 1,
    MW_COPY_STATUS_COPY_FAILED = 2,
    MW_COPY_STATUS_PTR_ACQUIRE_FAILED = 3,
    MW_COPY_STATUS_BAD_MODE = 4,
    MW_COPY_STATUS_BAD_DIRECTION = 5,
};

#ifdef __cplusplus
}
#endif
