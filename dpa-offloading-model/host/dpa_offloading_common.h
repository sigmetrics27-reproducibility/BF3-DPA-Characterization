#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    DPA_OFFLOADING_ELEMENTS = 16,
};

enum {
    DPA_OFFLOADING_KERNEL_COMPUTE = 0,
    DPA_OFFLOADING_KERNEL_POINTER_CHASING = 1,
};

enum {
    DPA_OFFLOADING_PTR_CHASING_BYTES = 64 * 1024,
    DPA_OFFLOADING_PTR_CHASING_STRIDE_BYTES = 64,
    DPA_OFFLOADING_PTR_CHASING_NODES =
        DPA_OFFLOADING_PTR_CHASING_BYTES / DPA_OFFLOADING_PTR_CHASING_STRIDE_BYTES,
};

struct dpa_offloading_ptr_chasing_node {
    uint64_t next;
    uint8_t pad[DPA_OFFLOADING_PTR_CHASING_STRIDE_BYTES - sizeof(uint64_t)];
};

#ifdef __cplusplus
static_assert(sizeof(struct dpa_offloading_ptr_chasing_node) ==
                  DPA_OFFLOADING_PTR_CHASING_STRIDE_BYTES,
              "pointer-chasing node must be 64 bytes");
#else
_Static_assert(sizeof(struct dpa_offloading_ptr_chasing_node) ==
                   DPA_OFFLOADING_PTR_CHASING_STRIDE_BYTES,
               "pointer-chasing node must be 64 bytes");
#endif

struct dpa_offloading_persistent_command {
    uint64_t shutdown_flag;
    uint32_t kernel_kind;
    uint32_t work_iterations;
    uint32_t inputs[DPA_OFFLOADING_ELEMENTS];
} __attribute__((aligned(64)));

struct dpa_offloading_persistent_doorbell {
    uint64_t request_seq;
} __attribute__((aligned(64)));

struct dpa_offloading_persistent_response {
    uint64_t done_seq;
    uint64_t kernel_cycles;
    uint32_t result;
} __attribute__((aligned(64)));

struct dpa_offloading_persistent_shm {
    struct dpa_offloading_persistent_command command;
    struct dpa_offloading_persistent_doorbell doorbell;
    struct dpa_offloading_persistent_response response;
} __attribute__((aligned(64)));

struct dpa_offloading_persistent_thread_arg {
    uint64_t dpa_ctx_handle;
    uint64_t shm_mmap_handle;
    uint64_t shm_addr;
    uint64_t ptr_chasing_base;
    uint64_t self_notify_handle;
    uint64_t last_served_seq;
};

#ifdef __cplusplus
}
#endif
