#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum dpa_external_ptr_copy_mode {
    DPA_EXTERNAL_PTR_COPY_MODE_H2D = 0,
    DPA_EXTERNAL_PTR_COPY_MODE_D2H = 1,
};

#ifdef __cplusplus
}
#endif
