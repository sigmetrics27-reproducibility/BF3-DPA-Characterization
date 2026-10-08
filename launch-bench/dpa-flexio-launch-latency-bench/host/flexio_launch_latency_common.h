#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// shared launch benchmark constants
enum {
    FLEXIO_LAUNCH_LATENCY_CLOCK_MHZ_DEFAULT = 1800,
};

// context shared by the RPC and event handler threads
struct flexio_event_activation_ctx {
    uint32_t activation_id;
    uint32_t reserved0;
    uint64_t delay_cycles;
    uint64_t trigger_seq; // incremented by the RPC before activate()
    uint64_t done_seq;    // set by the event handler when done
    uint64_t last_event_cycles;
};

#ifdef __cplusplus
}
#endif
