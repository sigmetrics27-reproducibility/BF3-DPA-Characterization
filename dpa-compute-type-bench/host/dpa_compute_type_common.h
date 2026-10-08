#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum dpa_compute_dtype {
    DPA_COMPUTE_DTYPE_UINT8 = 0,
    DPA_COMPUTE_DTYPE_UINT32 = 1,
    DPA_COMPUTE_DTYPE_UINT64 = 2,
    DPA_COMPUTE_DTYPE_FLOAT32 = 3,
    DPA_COMPUTE_DTYPE_FLOAT64 = 4,
    DPA_COMPUTE_DTYPE_COUNT
};

enum dpa_compute_op {
    DPA_COMPUTE_OP_ADD = 0,
    DPA_COMPUTE_OP_MUL = 1,
    DPA_COMPUTE_OP_DIV = 2,
    DPA_COMPUTE_OP_MIN = 3,
    DPA_COMPUTE_OP_MAX = 4,
    DPA_COMPUTE_OP_XOR = 5,
    DPA_COMPUTE_OP_AND = 6,
    DPA_COMPUTE_OP_COUNT
};

enum dpa_compute_mode {
    DPA_COMPUTE_MODE_LATENCY = 0,
    DPA_COMPUTE_MODE_THROUGHPUT = 1,
    DPA_COMPUTE_MODE_COUNT
};

#ifdef __cplusplus
}
#endif
