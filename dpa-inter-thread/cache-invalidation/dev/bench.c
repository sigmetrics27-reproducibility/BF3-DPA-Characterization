#include <doca_dpa_dev.h>
#include <doca_dpa_dev_sync_event.h>
#include <dpaintrin.h>
#include "bench.h"

struct guard { struct control *control; uint64_t start, budget; };

static int alive(struct guard *g)
{
    if (__atomic_load_n(&g->control->abort.value, __ATOMIC_RELAXED)) return 0;
    if (__dpa_thread_cycles() - g->start >= g->budget) {
        __atomic_store_n(&g->control->abort.value, STATUS_TIMEOUT, __ATOMIC_RELAXED);
        return 0;
    }
    return 1;
}

static int wait_for(uint64_t *flag, struct guard *g)
{
    unsigned spins = 0;
    while (__atomic_load_n(flag, __ATOMIC_ACQUIRE) == 0)
        if ((++spins & 1023U) == 0 && !alive(g)) return 0;
    return alive(g);
}

static void reader(const struct worker_arg *a, struct result *r, struct guard *g)
{
    struct control *c = g->control;
    (void)wait_for(&c->ready[1].value, g);
    // warm X in A's cache after B touches Y, before timing starts
    uint64_t *x = (uint64_t *)(uintptr_t)a->x;
    (void)__atomic_load_n(x, __ATOMIC_RELAXED);
    __atomic_store_n(&c->start.value, 1, __ATOMIC_RELEASE); // notify B
    uint64_t sum = 0, n = 0;
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    uint64_t start = __dpa_thread_cycles();
    if (alive(g)) {
        for (; n < a->reads; ++n) {
            if ((n & 1023U) == 0 && !alive(g)) break;
            sum += __atomic_load_n(x, __ATOMIC_RELAXED); // read X
        }
    }
    __dpa_thread_memory_fence(__DPA_RW, __DPA_RW);
    r->cycles = __dpa_thread_cycles() - start;
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    r->reads = n;
    r->checksum = sum;
    __atomic_store_n(&c->stop.value, 1, __ATOMIC_RELAXED);
    (void)wait_for(&c->done[1].value, g);
    __atomic_store_n(&c->done[0].value, 1, __ATOMIC_RELEASE);
}

static void writer(const struct worker_arg *a, struct result *r, struct guard *g)
{
    struct control *c = g->control;
    uint64_t *y = (uint64_t *)(uintptr_t)a->y;
    __atomic_store_n(y, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&c->ready[1].value, 1, __ATOMIC_RELEASE); // notify A
    (void)wait_for(&c->start.value, g); // wait for start
    uint64_t count = 0, missed = 0, first = 0, last = 0;
    uint64_t next = __dpa_thread_cycles() + a->period;
    unsigned spins = 0;
    while (__atomic_load_n(&c->stop.value, __ATOMIC_RELAXED) == 0) {
        uint64_t now = __dpa_thread_cycles();
        if (now >= next) {
            uint64_t skipped = (now - next) / a->period;
            missed += skipped;
            next += (skipped + 1) * a->period;
            // check stop again to avoid writes after the timed window
            if (__atomic_load_n(&c->stop.value, __ATOMIC_RELAXED)) break;
            __atomic_store_n(y, ++count, __ATOMIC_RELAXED); // disturb A
            if (count == 1) first = now;
            last = now;
        }
        if ((++spins & 1023U) == 0 && !alive(g)) break;
    }
    r->writes = count;
    r->missed = missed;
    r->first_write = first;
    r->last_write = last;
    r->final_y = __atomic_load_n(y, __ATOMIC_RELAXED);
    __atomic_store_n(&c->done[1].value, 1, __ATOMIC_RELEASE);
}

__dpa_global__ void bench_worker(uint64_t address)
{
    const struct worker_arg *a = (const struct worker_arg *)(uintptr_t)address;
    doca_dpa_dev_device_set(a->ctx);
    struct control *c = (struct control *)(uintptr_t)a->control;
    struct result *r = (struct result *)(uintptr_t)a->results + a->index;
    struct guard g = {c, __dpa_thread_cycles(), a->timeout_cycles};
    if (a->index == 0) reader(a, r, &g); else writer(a, r, &g);
    r->status = __atomic_load_n(&c->abort.value, __ATOMIC_RELAXED);
    __dpa_thread_memory_writeback();
    doca_dpa_dev_sync_event_update_add(a->event, 1);
    doca_dpa_dev_thread_reschedule();
}

__dpa_rpc__ uint64_t bench_start(uint64_t ctx, uint64_t handles_address)
{
    doca_dpa_dev_device_set(ctx);
    doca_dpa_dev_notification_completion_t *handles =
        (doca_dpa_dev_notification_completion_t *)(uintptr_t)handles_address;
    doca_dpa_dev_thread_notify(handles[0]);
    doca_dpa_dev_thread_notify(handles[1]);
    return 2;
}
