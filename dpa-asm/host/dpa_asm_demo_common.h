#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct dpa_asm_demo_cfg {
    uint64_t a;      // operand a (host -> device)
    uint64_t b;      // operand b (host -> device)
    uint64_t c;      // operand c (host -> device)
    uint64_t se_sum; // event handle for the sum
    uint64_t se_cyc; // event handle for the cycle counter
};

#ifdef __cplusplus
}
#endif
