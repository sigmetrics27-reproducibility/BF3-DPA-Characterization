#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct dpa_event_thread_ctx {
    uint64_t completion_se;
    uint64_t event_cycles_se;
    uint64_t delay_cycles;
};

#ifdef __cplusplus
}
#endif
