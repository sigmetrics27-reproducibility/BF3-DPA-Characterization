#include "dpa_offloading_dev_common.h"

static __always_inline uint32_t reduce_u32_add_compute_intensive(const volatile uint32_t *src,
                                                                 uint32_t adds_per_element)
{
    uint32_t acc = 0;
    for (uint32_t i = 0; i < DPA_OFFLOADING_ELEMENTS; ++i) {
        const uint32_t elem = src[i];
        uint32_t repeat = 0;
        for (; repeat + 16 <= adds_per_element; repeat += 16) {
            acc += elem ^ (acc & 1U);
            acc += elem ^ (acc & 1U);
            acc += elem ^ (acc & 1U);
            acc += elem ^ (acc & 1U);
            acc += elem ^ (acc & 1U);
            acc += elem ^ (acc & 1U);
            acc += elem ^ (acc & 1U);
            acc += elem ^ (acc & 1U);
            acc += elem ^ (acc & 1U);
            acc += elem ^ (acc & 1U);
            acc += elem ^ (acc & 1U);
            acc += elem ^ (acc & 1U);
            acc += elem ^ (acc & 1U);
            acc += elem ^ (acc & 1U);
            acc += elem ^ (acc & 1U);
            acc += elem ^ (acc & 1U);
        }
        for (; repeat < adds_per_element; ++repeat) {
            acc += elem ^ (acc & 1U);
        }
    }
    return acc;
}

__attribute__((aligned(64))) __dpa_global__ void dpa_offloading_kernel_compute_intensive(
    doca_dpa_dev_t dpa_ctx_handle,
    doca_dpa_dev_mmap_t host_mmap_handle,
    uint64_t host_region_addr,
    doca_dpa_dev_sync_event_t kernel_cycles_se,
    uint32_t adds_per_element)
{
    if (dpa_ctx_handle != 0) {
        doca_dpa_dev_device_set(dpa_ctx_handle);
    }
    doca_dpa_dev_uintptr_t host_ptr =
        doca_dpa_dev_mmap_get_external_ptr(host_mmap_handle, host_region_addr);
    volatile uint32_t *host_region = (volatile uint32_t *)(uintptr_t)host_ptr;

    __dpa_thread_window_read_inv();
    const uint64_t start_cycles = __dpa_thread_cycles();
    const uint32_t result = reduce_u32_add_compute_intensive(host_region, adds_per_element);
    const uint64_t end_cycles = __dpa_thread_cycles();

    host_region[DPA_OFFLOADING_ELEMENTS] = result;
    __dpa_thread_window_writeback();

    doca_dpa_dev_sync_event_update_set(kernel_cycles_se,
                                       dpa_offloading_elapsed_cycles(start_cycles, end_cycles));
}

__attribute__((aligned(64))) __dpa_rpc__ uint64_t dpa_offloading_kernel_pChasing_init_rpc(
    doca_dpa_dev_t dpa_ctx_handle,
    uint64_t ptr_chasing_array,
    uint64_t array_size,
    uint64_t stride)
{
    if (dpa_ctx_handle != 0U) {
        doca_dpa_dev_device_set(dpa_ctx_handle);
    }

    if (ptr_chasing_array == 0U || array_size != DPA_OFFLOADING_PTR_CHASING_BYTES ||
        stride != DPA_OFFLOADING_PTR_CHASING_STRIDE_BYTES) {
        return 1U;
    }

    struct dpa_offloading_ptr_chasing_node *nodes =
        (struct dpa_offloading_ptr_chasing_node *)(uintptr_t)ptr_chasing_array;
    for (uint32_t i = 0; i < DPA_OFFLOADING_PTR_CHASING_NODES; ++i) {
        const uint32_t next = (i + 1U) % DPA_OFFLOADING_PTR_CHASING_NODES;
        nodes[i].next = (uint64_t)(uintptr_t)&nodes[next];
    }
    __dpa_thread_memory_writeback();
    return 0U;
}

static __always_inline uint64_t pointer_chasing_device(uint64_t ptr_chasing_array, uint32_t ptr_chasing_iter)
{
    uint64_t dummy_var = 0;
    volatile struct dpa_offloading_ptr_chasing_node *node =
        (volatile struct dpa_offloading_ptr_chasing_node *)(uintptr_t)ptr_chasing_array;

    for (uint32_t i = 0; i < ptr_chasing_iter; ++i) {
        node = (volatile struct dpa_offloading_ptr_chasing_node *)(uintptr_t)node->next;
        dummy_var += node->next;
    }
    return dummy_var;
}


__attribute__((aligned(64))) __dpa_global__ void dpa_offloading_kernel_long_latency(
    doca_dpa_dev_t dpa_ctx_handle,
    doca_dpa_dev_sync_event_t kernel_cycles_se,
    uint64_t ptr_chasing_array,
    uint32_t ptr_chasing_iter)
{
    if (dpa_ctx_handle != 0) {
        doca_dpa_dev_device_set(dpa_ctx_handle);
    }

    const uint64_t start_cycles = __dpa_thread_cycles();
    const uint64_t dummy_var = pointer_chasing_device(ptr_chasing_array, ptr_chasing_iter);
    const uint64_t end_cycles = __dpa_thread_cycles();
    (void)dummy_var;

    doca_dpa_dev_sync_event_update_set(kernel_cycles_se,
                                       dpa_offloading_elapsed_cycles(start_cycles, end_cycles));
}

// persistent thread


#define DPA_OFFLOADING_PERSISTENT_CLOCK_HZ 1800000000ULL
#define DPA_OFFLOADING_PERSISTENT_REARM_SECONDS 5ULL
#define DPA_OFFLOADING_PERSISTENT_REARM_CYCLES \
    (DPA_OFFLOADING_PERSISTENT_REARM_SECONDS * DPA_OFFLOADING_PERSISTENT_CLOCK_HZ)

static __always_inline int dpa_offloading_persistent_slice_expired(uint64_t start_cycles)
{
    return (__dpa_thread_cycles() - start_cycles) >= DPA_OFFLOADING_PERSISTENT_REARM_CYCLES;
}

static __always_inline void dpa_offloading_persistent_rearm(uint64_t self_notify_handle)
{
    if (self_notify_handle != 0U) {
        doca_dpa_dev_thread_notify((doca_dpa_dev_notification_completion_t)self_notify_handle);
    }
    doca_dpa_dev_thread_reschedule();
}

__attribute__((aligned(64))) __dpa_global__ void dpa_offloading_persistent_thread(uint64_t arg)
{
    struct dpa_offloading_persistent_thread_arg *a =
        (struct dpa_offloading_persistent_thread_arg *)(uintptr_t)arg;

    if (a->dpa_ctx_handle != 0U) {
        doca_dpa_dev_device_set((doca_dpa_dev_t)a->dpa_ctx_handle);
    }

    volatile struct dpa_offloading_persistent_shm *shm =
        (volatile struct dpa_offloading_persistent_shm *)(uintptr_t)
            doca_dpa_dev_mmap_get_external_ptr((doca_dpa_dev_mmap_t)a->shm_mmap_handle, a->shm_addr);

    // keep last_served_seq across rearms to catch requests sent during rescheduling
    uint64_t last_seq = a->last_served_seq;

    const uint64_t slice_start = __dpa_thread_cycles();
    while (!dpa_offloading_persistent_slice_expired(slice_start)) {
        __dpa_thread_window_read_inv();
        if (shm->command.shutdown_flag != 0U) {
            // host asked us to stop, so skip rescheduling
            doca_dpa_dev_thread_finish();
            return;
        }
        const uint64_t seq = shm->doorbell.request_seq;
        if (seq == last_seq) {
            continue;
        }
        last_seq = seq;
        a->last_served_seq = seq;

        __dpa_thread_window_read_inv();
        const uint64_t work_start = __dpa_thread_cycles();
        uint32_t result = 0;
        if (shm->command.kernel_kind == DPA_OFFLOADING_KERNEL_POINTER_CHASING) {
            result = (uint32_t)pointer_chasing_device(a->ptr_chasing_base, shm->command.work_iterations);
        } else {
            result = reduce_u32_add_compute_intensive(shm->command.inputs, shm->command.work_iterations);
        }
        const uint64_t work_end = __dpa_thread_cycles();
        shm->response.result = result;
        shm->response.kernel_cycles = dpa_offloading_elapsed_cycles(work_start, work_end);
        shm->response.done_seq = seq;
        __dpa_thread_window_writeback();
    }

    dpa_offloading_persistent_rearm(a->self_notify_handle);
}

__attribute__((aligned(64))) __dpa_rpc__ uint64_t dpa_offloading_persistent_kickstart_rpc(
    doca_dpa_dev_t dpa_ctx_handle,
    doca_dpa_dev_notification_completion_t notify_handle)
{
    if (dpa_ctx_handle != 0U) {
        doca_dpa_dev_device_set(dpa_ctx_handle);
    }
    if (notify_handle != 0U) {
        doca_dpa_dev_thread_notify(notify_handle);
    }
    return 0U;
}
