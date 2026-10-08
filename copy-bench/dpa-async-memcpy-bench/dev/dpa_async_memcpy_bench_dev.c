#include <doca_dpa_dev.h>
#include <doca_dpa_dev_buf.h>
#include <doca_dpa_dev_sync_event.h>
#include <dpaintrin.h>

#include "../host/dpa_async_memcpy_common.h"

static __always_inline uint64_t elapsed_cycles(uint64_t start, uint64_t end)
{
    if (end >= start) {
        return end - start;
    }
    return ((1ULL << 32) - 1ULL) - start + end;
}

static __always_inline void wait_one_completion(doca_dpa_dev_completion_t comp_handle)
{
    doca_dpa_dev_completion_element_t comp_element;

    while (!doca_dpa_dev_get_completion(comp_handle, &comp_element)) {
    }

    doca_dpa_dev_completion_ack(comp_handle, 1);
}

static __always_inline void post_memcpy_iter(const struct dpa_async_memcpy_thread_cmd *cmd)
{
    if (cmd->mode == DPA_ASYNC_MEMCPY_MODE_H2D) {
        doca_dpa_dev_post_memcpy((doca_dpa_dev_async_ops_t)cmd->async_ops_handle,
                                 (doca_dpa_dev_mmap_t)cmd->local_mmap_handle,
                                 cmd->local_addr,
                                 (doca_dpa_dev_mmap_t)cmd->host_mmap_handle,
                                 cmd->host_src_addr,
                                 cmd->payload_bytes,
                                 DOCA_DPA_DEV_SUBMIT_FLAG_FLUSH);
    } else {
        doca_dpa_dev_post_memcpy((doca_dpa_dev_async_ops_t)cmd->async_ops_handle,
                                 (doca_dpa_dev_mmap_t)cmd->host_mmap_handle,
                                 cmd->host_dst_addr,
                                 (doca_dpa_dev_mmap_t)cmd->local_mmap_handle,
                                 cmd->local_addr,
                                 cmd->payload_bytes,
                                 DOCA_DPA_DEV_SUBMIT_FLAG_FLUSH);
    }
}

__dpa_global__ void dpa_async_memcpy_thread(uint64_t arg)
{
    struct dpa_async_memcpy_thread_cmd *cmd = (struct dpa_async_memcpy_thread_cmd *)arg;
    const uint64_t total_iters = (uint64_t)cmd->warmup_iters + (uint64_t)cmd->measure_iters;
    uint64_t start_cycles = 0;
    uint64_t end_cycles = 0;

    cmd->cycles = 0;
    cmd->status = DPA_ASYNC_MEMCPY_STATUS_OK;

    if (cmd->dpa_ctx_handle != 0) {
        doca_dpa_dev_device_set((doca_dpa_dev_t)cmd->dpa_ctx_handle);
    }

    if (cmd->payload_bytes == 0) {
        cmd->status = DPA_ASYNC_MEMCPY_STATUS_BAD_ARGS;
        doca_dpa_dev_sync_event_update_set((doca_dpa_dev_sync_event_t)cmd->completion_se, cmd->seq);
        doca_dpa_dev_thread_reschedule();
        return;
    }

    if (cmd->measure_iters == 0) {
        doca_dpa_dev_sync_event_update_set((doca_dpa_dev_sync_event_t)cmd->completion_se, cmd->seq);
        doca_dpa_dev_thread_reschedule();
        return;
    }

    if (cmd->mode != DPA_ASYNC_MEMCPY_MODE_H2D && cmd->mode != DPA_ASYNC_MEMCPY_MODE_D2H) {
        cmd->status = DPA_ASYNC_MEMCPY_STATUS_BAD_MODE;
        doca_dpa_dev_sync_event_update_set((doca_dpa_dev_sync_event_t)cmd->completion_se, cmd->seq);
        doca_dpa_dev_thread_reschedule();
        return;
    }

    for (uint64_t iter = 0; iter < total_iters; ++iter) {
        if (iter == (uint64_t)cmd->warmup_iters) {
            start_cycles = __dpa_thread_cycles();
        }

        doca_dpa_dev_completion_request_notification((doca_dpa_dev_completion_t)cmd->completion_handle);
        post_memcpy_iter(cmd);
        wait_one_completion((doca_dpa_dev_completion_t)cmd->completion_handle);
    }

    end_cycles = __dpa_thread_cycles();
    cmd->cycles = elapsed_cycles(start_cycles, end_cycles);

    doca_dpa_dev_sync_event_update_set((doca_dpa_dev_sync_event_t)cmd->completion_se, cmd->seq);
    doca_dpa_dev_thread_reschedule();
}

__dpa_rpc__ uint64_t dpa_async_memcpy_notify_rpc(doca_dpa_dev_t dpa_ctx_handle,
                                                 doca_dpa_dev_notification_completion_t notify_handle)
{
    if (dpa_ctx_handle != 0) {
        doca_dpa_dev_device_set(dpa_ctx_handle);
    }

    doca_dpa_dev_thread_notify(notify_handle);
    return 0;
}
