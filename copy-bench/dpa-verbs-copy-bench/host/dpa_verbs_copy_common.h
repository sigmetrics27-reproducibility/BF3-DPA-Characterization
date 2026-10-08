#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum dpa_verbs_copy_mode {
    DPA_VERBS_COPY_MODE_READ = 0,
    DPA_VERBS_COPY_MODE_WRITE = 1,
    DPA_VERBS_COPY_MODE_SEND = 2,
};

#ifdef __cplusplus
}
#endif
