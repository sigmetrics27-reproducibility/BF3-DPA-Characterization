#include <doca_dpa_dev.h>
#include <doca_dpa_dev_verbs.h>
#include <dpaintrin.h>

#include "../host/dpa_verbs_copy_common.h"

static __always_inline void post_send_wr(doca_dpa_dev_verbs_qp_t qp_handle,
                                          struct doca_dpa_dev_verbs_send_wr *send_wr,
                                          struct doca_dpa_dev_verbs_sge *sge,
                                          enum doca_dpa_dev_verbs_send_wr_opcode opcode,
                                          uint64_t remote_addr,
                                          uint32_t remote_rkey)
{
    __builtin_memset(send_wr, 0, sizeof(*send_wr));
    doca_dpa_dev_verbs_send_wr_set_sg_list(send_wr, sge);
    doca_dpa_dev_verbs_send_wr_set_sg_num_sge(send_wr, 1);
    doca_dpa_dev_verbs_send_wr_set_send_flags(send_wr, DOCA_DPA_DEV_VERBS_SEND_WR_FLAGS_SIGNALED);
    doca_dpa_dev_verbs_send_wr_set_opcode(send_wr, opcode);
    doca_dpa_dev_verbs_send_wr_set_fence_mode(send_wr, DOCA_DPA_DEV_VERBS_SEND_WR_FM_NO_FENCE);

    if (opcode == DOCA_DPA_DEV_VERBS_SEND_WR_OPCODE_READ ||
        opcode == DOCA_DPA_DEV_VERBS_SEND_WR_OPCODE_WRITE) {
        doca_dpa_dev_verbs_send_wr_set_rdma_remote_addr(send_wr, remote_addr);
        doca_dpa_dev_verbs_send_wr_set_rdma_rkey(send_wr, remote_rkey);
    }

    doca_dpa_dev_verbs_qp_post_send_wr(qp_handle, send_wr);
    doca_dpa_dev_verbs_qp_commit_send(qp_handle);
}

static __always_inline void wait_one_completion(doca_dpa_dev_completion_t comp_handle)
{
    doca_dpa_dev_completion_element_t comp_element;

    while (!doca_dpa_dev_get_completion(comp_handle, &comp_element)) {
    }

    doca_dpa_dev_completion_ack(comp_handle, 1);
}

__dpa_rpc__ uint64_t dpa_copy_run_rpc(doca_dpa_dev_t dpa_ctx_handle,
                                      doca_dpa_dev_verbs_qp_t dpa_verbs_qp_handle,
                                      doca_dpa_dev_completion_t dpa_comp_handle,
                                      doca_dpa_dev_uintptr_t local_buf_addr,
                                      uint32_t local_lkey,
                                      uint64_t remote_base_addr,
                                      uint32_t remote_rkey,
                                      uint32_t mode,
                                      uint32_t warmup_iters,
                                      uint32_t measure_iters,
                                      uint32_t payload_bytes)
{
    struct doca_dpa_dev_verbs_send_wr send_wr;
    struct doca_dpa_dev_verbs_sge sge;
    uint64_t total_iters = (uint64_t)warmup_iters + (uint64_t)measure_iters;
    uint64_t start_cycles = 0;
    uint64_t end_cycles = 0;

    if (dpa_ctx_handle != 0) {
        doca_dpa_dev_device_set(dpa_ctx_handle);
    }

    if (payload_bytes == 0) {
        payload_bytes = 1;
    }

    sge.addr = local_buf_addr;
    sge.lkey = local_lkey;
    sge.length = payload_bytes;

    for (uint64_t iter = 0; iter < total_iters; ++iter) {
        if (iter == (uint64_t)warmup_iters) {
            start_cycles = __dpa_thread_cycles();
        }

        if (mode == DPA_VERBS_COPY_MODE_READ) {
            post_send_wr(dpa_verbs_qp_handle,
                         &send_wr,
                         &sge,
                         DOCA_DPA_DEV_VERBS_SEND_WR_OPCODE_READ,
                         remote_base_addr,
                         remote_rkey);
        } else if (mode == DPA_VERBS_COPY_MODE_WRITE) {
            post_send_wr(dpa_verbs_qp_handle,
                         &send_wr,
                         &sge,
                         DOCA_DPA_DEV_VERBS_SEND_WR_OPCODE_WRITE,
                         remote_base_addr,
                         remote_rkey);
        } else {
            post_send_wr(dpa_verbs_qp_handle,
                         &send_wr,
                         &sge,
                         DOCA_DPA_DEV_VERBS_SEND_WR_OPCODE_SEND,
                         0,
                         0);
        }

        wait_one_completion(dpa_comp_handle);
    }

    end_cycles = __dpa_thread_cycles();

    if (measure_iters == 0) {
        return 0;
    }
    if (end_cycles >= start_cycles) {
        return end_cycles - start_cycles;
    }

    return ((1ULL << 32) - 1ULL) - start_cycles + end_cycles;
}
