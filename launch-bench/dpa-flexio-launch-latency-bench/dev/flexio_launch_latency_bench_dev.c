#include <libflexio-dev/flexio_dev_ver.h>

#define FLEXIO_DEV_VER_USED FLEXIO_DEV_VER(25, 10, 0)

#include <libflexio-dev/flexio_dev.h>
#include <dpaintrin.h>
#include <stdint.h>

#include "../host/flexio_launch_latency_common.h"

static inline uint64_t elapsed_cycles(uint64_t start, uint64_t end)
{
    if (end >= start) {
        return end - start;
    }

    return ((1ULL << 32) - start) + end;
}

static inline void busy_wait_cycles(uint64_t delay_cycles)
{
    if (delay_cycles == 0) {
        return;
    }

    const uint64_t wait_start = __dpa_thread_cycles();
    while (elapsed_cycles(wait_start, __dpa_thread_cycles()) < delay_cycles) {
    }
}

__dpa_rpc__ uint64_t flexio_latency_rpc_empty(uint64_t unused)
{
    (void)unused;

    const uint64_t start = __dpa_thread_cycles();
    const uint64_t end = __dpa_thread_cycles();
    return elapsed_cycles(start, end);
}

__dpa_rpc__ uint64_t flexio_latency_rpc_delay(uint64_t delay_cycles)
{
    const uint64_t start = __dpa_thread_cycles();
    busy_wait_cycles(delay_cycles);
    const uint64_t end = __dpa_thread_cycles();
    return elapsed_cycles(start, end);
}

flexio_dev_event_handler_t flexio_latency_event_handler;
__dpa_global__ void flexio_latency_event_handler(uint64_t ctx_daddr)
{
    volatile struct flexio_event_activation_ctx *ctx =
        (volatile struct flexio_event_activation_ctx *)(uintptr_t)ctx_daddr;
    const uint64_t expected_seq = ctx->trigger_seq;
    const uint64_t delay_cycles = ctx->delay_cycles;

    const uint64_t start = __dpa_thread_cycles();
    busy_wait_cycles(delay_cycles);
    const uint64_t end = __dpa_thread_cycles();

    ctx->last_event_cycles = elapsed_cycles(start, end);
    ctx->done_seq = expected_seq;
    __dpa_thread_memory_writeback();

    flexio_dev_thread_reschedule();
}

__dpa_rpc__ uint64_t flexio_latency_rpc_trigger_event(uint64_t ctx_daddr)
{
    volatile struct flexio_event_activation_ctx *ctx =
        (volatile struct flexio_event_activation_ctx *)(uintptr_t)ctx_daddr;
    const uint64_t next_seq = ctx->trigger_seq + 1;

    ctx->trigger_seq = next_seq;
    __dpa_thread_memory_writeback();

    // time activation to completion inside the RPC
    const uint64_t start = __dpa_thread_cycles();
    flexio_dev_event_handler_activate(ctx->activation_id);

    while (ctx->done_seq < next_seq) {
        __dpa_thread_memory_fence(__DPA_R, __DPA_R);
    }

    const uint64_t end = __dpa_thread_cycles();
    return elapsed_cycles(start, end);
}
