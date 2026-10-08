#include <doca_dpa_dev.h>
#include <doca_dpa_dev_sync_event.h>
#include <dpaintrin.h>

#include "dpa_arch_probe_common.h"

#define FINISH(c, acc, c0, t0, i0)                                      \
    do {                                                                \
        const uint64_t c1 = __dpa_thread_cycles();                      \
        const uint64_t t1 = __dpa_thread_time();                        \
        const uint64_t i1 = __dpa_thread_inst_ret();                    \
        doca_dpa_dev_sync_event_update_set((c)->se_cyc, c1 - (c0));     \
        doca_dpa_dev_sync_event_update_set((c)->se_time, t1 - (t0));    \
        doca_dpa_dev_sync_event_update_set((c)->se_inst, i1 - (i0));    \
        doca_dpa_dev_sync_event_update_set((c)->se_sink, (acc));        \
        doca_dpa_dev_thread_reschedule();                               \
    } while (0)

#define COMPUTE_ROUND(delta)       \
    do {                           \
        x += s + (delta);          \
        a0 += x; a1 += x + 1;      \
        a2 += x + 2; a3 += x + 3;  \
        a4 += x + 4; a5 += x + 5;  \
        a6 += x + 6; a7 += x + 7;  \
        a8 += x + 8; a9 += x + 9;  \
        aa += x + 10; ab += x + 11;\
        ac += x + 12; ad += x + 13;\
        ae += x + 14; af += x + 15;\
    } while (0)

static void setup_device(struct dpa_aff_cfg *c)
{
    if (c->dpa_handle != 0U) {
        doca_dpa_dev_device_set((doca_dpa_dev_t)c->dpa_handle);
    }
}

__dpa_global__ void k_mem_random(uint64_t arg)
{
    struct dpa_aff_cfg *c = (struct dpa_aff_cfg *)(uintptr_t)arg;
    if (c == NULL || c->M == 0 || c->data_addr == 0U) {
        return;
    }
    setup_device(c);

    const uint64_t c0 = __dpa_thread_cycles();
    const uint64_t t0 = __dpa_thread_time();
    const uint64_t i0 = __dpa_thread_inst_ret();

    volatile uint64_t *table = (volatile uint64_t *)(uintptr_t)c->data_addr;
    uint64_t idx = c->start_index & c->data_mask;
    uint64_t acc = c->seed;
    uint64_t k;
    for (k = 0; k < c->M; ++k) {
        idx = table[idx] & c->data_mask;
        acc ^= idx + 0x9e3779b97f4a7c15ULL;
    }

    FINISH(c, acc ^ idx, c0, t0, i0);
}

__dpa_global__ void k_mem_stream(uint64_t arg)
{
    struct dpa_aff_cfg *c = (struct dpa_aff_cfg *)(uintptr_t)arg;
    if (c == NULL || c->M == 0 || c->data_addr == 0U) {
        return;
    }
    setup_device(c);

    const uint64_t c0 = __dpa_thread_cycles();
    const uint64_t t0 = __dpa_thread_time();
    const uint64_t i0 = __dpa_thread_inst_ret();

    volatile uint64_t *data = (volatile uint64_t *)(uintptr_t)c->data_addr;
    uint64_t idx = c->start_index & c->data_mask;
    uint64_t a = c->seed;
    uint64_t b = ~c->seed;
    uint64_t d = c->seed ^ 0xaaaaaaaaaaaaaaaaULL;
    uint64_t k;
    for (k = 0; k < c->M; ++k) {
        const uint64_t v0 = data[idx];
        idx = (idx + c->data_stride) & c->data_mask;
        const uint64_t v1 = data[idx];
        idx = (idx + c->data_stride) & c->data_mask;
        a += v0;
        b ^= v1;
        d += (a ^ b) + 0x517cc1b727220a95ULL;
    }

    FINISH(c, a ^ b ^ d ^ idx, c0, t0, i0);
}

__dpa_global__ void k_compute(uint64_t arg)
{
    struct dpa_aff_cfg *c = (struct dpa_aff_cfg *)(uintptr_t)arg;
    if (c == NULL || c->M == 0) {
        return;
    }
    setup_device(c);

    const uint64_t c0 = __dpa_thread_cycles();
    const uint64_t t0 = __dpa_thread_time();
    const uint64_t i0 = __dpa_thread_inst_ret();

    uint64_t x = c->seed;
    uint64_t a0 = c->seed ^ 0x01, a1 = c->seed ^ 0x02, a2 = c->seed ^ 0x03, a3 = c->seed ^ 0x04;
    uint64_t a4 = c->seed ^ 0x05, a5 = c->seed ^ 0x06, a6 = c->seed ^ 0x07, a7 = c->seed ^ 0x08;
    uint64_t a8 = c->seed ^ 0x09, a9 = c->seed ^ 0x0a, aa = c->seed ^ 0x0b, ab = c->seed ^ 0x0c;
    uint64_t ac = c->seed ^ 0x0d, ad = c->seed ^ 0x0e, ae = c->seed ^ 0x0f, af = c->seed ^ 0x10;
    volatile uint64_t *salt = (volatile uint64_t *)&c->seed;
    uint64_t k;
    for (k = 0; k < c->M; ++k) {
        const uint64_t s = *salt;
        COMPUTE_ROUND(0x9e3779b97f4a7c15ULL);
        COMPUTE_ROUND(0xbf58476d1ce4e5b9ULL);
        COMPUTE_ROUND(0x94d049bb133111ebULL);
        COMPUTE_ROUND(0xd6e8feb86659fd93ULL);
    }

    FINISH(c, a0 ^ a1 ^ a2 ^ a3 ^ a4 ^ a5 ^ a6 ^ a7 ^ a8 ^ a9 ^ aa ^ ab ^ ac ^ ad ^ ae ^ af ^ x,
           c0, t0, i0);
}

__dpa_rpc__ uint64_t k_kickoff(doca_dpa_dev_t dpa_ctx_handle,
                               doca_dpa_dev_uintptr_t notify_handles_addr,
                               uint32_t count)
{
    if (dpa_ctx_handle != 0U) {
        doca_dpa_dev_device_set(dpa_ctx_handle);
    }
    if (notify_handles_addr == 0U || count == 0U) {
        return 0U;
    }
    doca_dpa_dev_notification_completion_t *handles =
        (doca_dpa_dev_notification_completion_t *)(uintptr_t)notify_handles_addr;
    uint32_t i;
    for (i = 0; i < count; ++i) {
        doca_dpa_dev_thread_notify(handles[i]);
    }
    return (uint64_t)count;
}
