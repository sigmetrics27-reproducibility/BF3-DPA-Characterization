#include <doca_dpa_dev.h>
#include <doca_dpa_dev_comch_msgq.h>
#include <dpaintrin.h>

#include "../host/dpa_comch_copy_common.h"

static __always_inline void wait_one_completion(doca_dpa_dev_completion_t comp_handle)
{
    doca_dpa_dev_completion_element_t comp_element;

    while (!doca_dpa_dev_get_completion(comp_handle, &comp_element)) {
    }
    doca_dpa_dev_completion_ack(comp_handle, 1);
}

static __always_inline void wait_completions(doca_dpa_dev_completion_t comp_handle, uint64_t count)
{
    for (uint64_t i = 0; i < count; ++i) {
        wait_one_completion(comp_handle);
    }
}

static __always_inline uint64_t post_dma_copy_iter(doca_dpa_dev_comch_producer_t producer_handle,
                                                   doca_dpa_dev_completion_t producer_comp_handle,
                                                   uint32_t consumer_id,
                                                   doca_dpa_dev_mmap_t host_mmap_handle,
                                                   uint64_t host_src_addr,
                                                   uint64_t host_dst_addr,
                                                   doca_dpa_dev_mmap_t local_mmap_handle,
                                                   uint64_t local_addr,
                                                   uint32_t direction,
                                                   uint32_t iter,
                                                   uint32_t payload_bytes,
                                                   uint32_t imm_bytes,
                                                   uint32_t op_bytes)
{
    struct dpa_comch_imm_payload imm_payload;
    uint8_t imm_storage[sizeof(imm_payload)];
    uint64_t posted = 0;

    if (imm_bytes > sizeof(imm_storage)) {
        imm_bytes = sizeof(imm_storage);
    }

    imm_payload.payload_bytes = payload_bytes;
    imm_payload.iter = iter;
    __builtin_memcpy(imm_storage, &imm_payload, sizeof(imm_payload));

    for (uint32_t offset = 0; offset < payload_bytes; offset += op_bytes) {
        const uint32_t chunk = (payload_bytes - offset < op_bytes) ? (payload_bytes - offset) : op_bytes;

        while (doca_dpa_dev_comch_producer_is_consumer_empty(producer_handle, consumer_id) != 0) {
        }

        doca_dpa_dev_completion_request_notification(producer_comp_handle);

        if (direction == DPA_COMCH_COPY_DIR_H2D) {
            doca_dpa_dev_comch_producer_dma_copy(producer_handle,
                                                 consumer_id,
                                                 local_mmap_handle,
                                                 local_addr + offset,
                                                 host_mmap_handle,
                                                 host_src_addr + offset,
                                                 chunk,
                                                 imm_storage,
                                                 imm_bytes,
                                                 DOCA_DPA_DEV_SUBMIT_FLAG_FLUSH);
        } else {
            doca_dpa_dev_comch_producer_dma_copy(producer_handle,
                                                 consumer_id,
                                                 host_mmap_handle,
                                                 host_dst_addr + offset,
                                                 local_mmap_handle,
                                                 local_addr + offset,
                                                 chunk,
                                                 imm_storage,
                                                 imm_bytes,
                                                 DOCA_DPA_DEV_SUBMIT_FLAG_FLUSH);
        }

        ++posted;
    }

    return posted;
}

__dpa_rpc__ uint64_t dpa_comch_copy_run_rpc(doca_dpa_dev_t dpa_ctx_handle,
                                            doca_dpa_dev_comch_producer_t producer_handle,
                                            doca_dpa_dev_completion_t producer_comp_handle,
                                            uint32_t consumer_id,
                                            doca_dpa_dev_mmap_t host_mmap_handle,
                                            uint64_t host_src_addr,
                                            uint64_t host_dst_addr,
                                            doca_dpa_dev_mmap_t local_mmap_handle,
                                            uint64_t local_addr,
                                            uint32_t direction,
                                            uint32_t warmup_iters,
                                            uint32_t measure_iters,
                                            uint32_t payload_bytes,
                                            uint32_t imm_bytes,
                                            uint32_t op_bytes)
{
    uint64_t start_cycles = 0;
    uint64_t end_cycles = 0;

    if (payload_bytes == 0 || measure_iters == 0 || op_bytes == 0) {
        return 0;
    }

    if (dpa_ctx_handle != 0) {
        doca_dpa_dev_device_set(dpa_ctx_handle);
    }

    if (warmup_iters != 0) {
        for (uint32_t iter = 0; iter < warmup_iters; ++iter) {
            const uint64_t posted = post_dma_copy_iter(producer_handle,
                                                       producer_comp_handle,
                                                       consumer_id,
                                                       host_mmap_handle,
                                                       host_src_addr,
                                                       host_dst_addr,
                                                       local_mmap_handle,
                                                       local_addr,
                                                       direction,
                                                       iter,
                                                       payload_bytes,
                                                       imm_bytes,
                                                       op_bytes);
            wait_completions(producer_comp_handle, posted);
        }
    }

    start_cycles = __dpa_thread_cycles();
    for (uint32_t iter = 0; iter < measure_iters; ++iter) {
        const uint64_t posted = post_dma_copy_iter(producer_handle,
                                                   producer_comp_handle,
                                                   consumer_id,
                                                   host_mmap_handle,
                                                   host_src_addr,
                                                   host_dst_addr,
                                                   local_mmap_handle,
                                                   local_addr,
                                                   direction,
                                                   warmup_iters + iter,
                                                   payload_bytes,
                                                   imm_bytes,
                                                   op_bytes);
        wait_completions(producer_comp_handle, posted);
    }
    end_cycles = __dpa_thread_cycles();

    if (end_cycles >= start_cycles) {
        return end_cycles - start_cycles;
    }
    return ((1ULL << 32) - 1ULL) - start_cycles + end_cycles;
}
