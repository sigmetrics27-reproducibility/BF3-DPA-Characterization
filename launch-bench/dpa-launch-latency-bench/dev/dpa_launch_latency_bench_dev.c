#include <doca_dpa_dev.h>
#include <doca_dpa_dev_sync_event.h>
#include <dpaintrin.h>
#include "dpa_launch_latency_common.h"

static __always_inline uint64_t elapsed_cycles(uint64_t start, uint64_t end)
{
    if (end >= start) {
        return end - start;
    }

    return (1ULL << 32) - start + end;
}

static __always_inline void busy_wait_cycles(uint64_t delay_cycles)
{
    if (delay_cycles == 0) {
        return;
    }

    const uint64_t wait_start = __dpa_thread_cycles();
    while (elapsed_cycles(wait_start, __dpa_thread_cycles()) < delay_cycles) {
    }
}

__dpa_rpc__ uint64_t dpa_rpc_delay(doca_dpa_dev_t dpa_ctx_handle, uint64_t delay_cycles)
{
    const uint64_t start = __dpa_thread_cycles();

    if (dpa_ctx_handle != 0) {
        doca_dpa_dev_device_set(dpa_ctx_handle);
    }

    busy_wait_cycles(delay_cycles);

    const uint64_t end = __dpa_thread_cycles();
    return elapsed_cycles(start, end);
}

__dpa_global__ void dpa_kernel_delay(doca_dpa_dev_t dpa_ctx_handle,
                                     doca_dpa_dev_sync_event_t kernel_cycles_se,
                                     uint64_t delay_cycles)
{
    if (dpa_ctx_handle != 0) {
        doca_dpa_dev_device_set(dpa_ctx_handle);
    }

    const uint64_t start = __dpa_thread_cycles();
    busy_wait_cycles(delay_cycles);
    const uint64_t end = __dpa_thread_cycles();

    doca_dpa_dev_sync_event_update_set(kernel_cycles_se, elapsed_cycles(start, end));
}

__dpa_global__ void dpa_event_handler_thread(void)
{
    struct dpa_event_thread_ctx *ctx = (struct dpa_event_thread_ctx *)doca_dpa_dev_thread_get_local_storage();

    const uint64_t start = __dpa_thread_cycles();
    busy_wait_cycles(ctx->delay_cycles);
    const uint64_t end = __dpa_thread_cycles();

    doca_dpa_dev_sync_event_update_set(ctx->event_cycles_se, elapsed_cycles(start, end));
    doca_dpa_dev_sync_event_update_add(ctx->completion_se, 1);
    doca_dpa_dev_thread_reschedule();
}

__dpa_rpc__ uint64_t dpa_rpc_event_notify(doca_dpa_dev_t dpa_ctx_handle,
                                          doca_dpa_dev_notification_completion_t notify_handle)
{
    const uint64_t start = __dpa_thread_cycles();

    if (dpa_ctx_handle != 0) {
        doca_dpa_dev_device_set(dpa_ctx_handle);
    }

    doca_dpa_dev_thread_notify(notify_handle);

    const uint64_t end = __dpa_thread_cycles();
    return elapsed_cycles(start, end);
}

__dpa_global__ void dpa_kernel_event_trigger_timed(doca_dpa_dev_t dpa_ctx_handle,
                                                   doca_dpa_dev_notification_completion_t notify_handle,
                                                   doca_dpa_dev_sync_event_t completion_se,
                                                   doca_dpa_dev_sync_event_t full_cycles_se)
{
    if (dpa_ctx_handle != 0) {
        doca_dpa_dev_device_set(dpa_ctx_handle);
    }

    uint64_t completion_prev = 0;
    doca_dpa_dev_sync_event_get(completion_se, &completion_prev);

    const uint64_t start = __dpa_thread_cycles();
    doca_dpa_dev_thread_notify(notify_handle);
    doca_dpa_dev_sync_event_wait_gt(completion_se, completion_prev, UINT64_MAX);
    const uint64_t end = __dpa_thread_cycles();

    doca_dpa_dev_sync_event_update_set(full_cycles_se, elapsed_cycles(start, end));
}
