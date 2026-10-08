#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum dpa_comch_copy_direction {
    DPA_COMCH_COPY_DIR_H2D = 0,
    DPA_COMCH_COPY_DIR_D2H = 1,
};

struct dpa_comch_imm_payload {
    uint32_t payload_bytes;
    uint32_t iter;
};

#ifdef __cplusplus
}
#endif
