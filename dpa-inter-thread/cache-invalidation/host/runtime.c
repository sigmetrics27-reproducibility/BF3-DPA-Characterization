#define _POSIX_C_SOURCE 200809L
#include "runtime.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <doca_error.h>
#include <doca_log.h>

extern struct doca_dpa_app *DPA_APP_NAME;
extern doca_dpa_func_t bench_worker, bench_start;

#define CHECK(call) do { doca_error_t e_ = (call); if (e_ != DOCA_SUCCESS) { \
    fprintf(stderr, "%s: %s\n", #call, doca_error_get_descr(e_)); return -1; } } while (0)

static double seconds(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static int allocate(struct runtime *r, struct buffer *b, size_t bytes)
{
    b->bytes = bytes;
    CHECK(doca_dpa_mem_alloc(r->dpa, bytes + SLOT_BYTES - 1, &b->raw));
    b->ptr = (b->raw + SLOT_BYTES - 1) & ~(uint64_t)(SLOT_BYTES - 1);
    CHECK(doca_dpa_memset(r->dpa, b->ptr, 0, bytes));
    return 0;
}

int runtime_open(const struct config *c, struct runtime *r)
{
    struct doca_devinfo **list = NULL;
    struct doca_log_backend *log = NULL;
    uint32_t count = 0;
    CHECK(doca_log_backend_create_with_file(stderr, &log));
    CHECK(doca_devinfo_create_list(&list, &count));
    doca_error_t opened = DOCA_ERROR_NOT_FOUND;
    for (uint32_t i = 0; i < count; ++i) {
        char name[DOCA_DEVINFO_IBDEV_NAME_SIZE + 1] = {0};
        if (doca_devinfo_get_ibdev_name(list[i], name, DOCA_DEVINFO_IBDEV_NAME_SIZE) == DOCA_SUCCESS &&
            strcmp(name, c->device) == 0) {
            opened = doca_dev_open(list[i], &r->dev);
            break;
        }
    }
    (void)doca_devinfo_destroy_list(list);
    CHECK(opened);
    CHECK(doca_dpa_create(r->dev, &r->dpa));
    CHECK(doca_dpa_set_app(r->dpa, DPA_APP_NAME));
    CHECK(doca_dpa_start(r->dpa));
    unsigned int cores = 0, eus = 0, total = 0;
    unsigned long long watchdog = 0;
    doca_dpa_dev_t ctx;
    doca_dpa_dev_sync_event_t event;
    CHECK(doca_dpa_get_core_num(r->dpa, &cores));
    CHECK(doca_dpa_get_num_eus_per_core(r->dpa, &eus));
    CHECK(doca_dpa_get_total_num_eus_available(r->dpa, &total));
    CHECK(doca_dpa_get_kernel_max_run_time(r->dpa, &watchdog));
    CHECK(doca_dpa_get_dpa_handle(r->dpa, &ctx));
    fprintf(stderr, "sdk_cores=%u sdk_eus_per_core=%u sdk_total_eus=%u watchdog_seconds=%llu\n",
            cores, eus, total, watchdog);
    CHECK(doca_sync_event_create(&r->event));
    CHECK(doca_sync_event_add_publisher_location_cpu(r->event, r->dev));
    CHECK(doca_sync_event_add_subscriber_location_dpa(r->event, r->dpa));
    CHECK(doca_sync_event_start(r->event));
    CHECK(doca_sync_event_get_dpa_handle(r->event, r->dpa, &event));
    if (allocate(r, &r->control, sizeof(struct control)) ||
        allocate(r, &r->data, 2 * SLOT_BYTES) ||
        allocate(r, &r->results, 2 * sizeof(struct result)) ||
        allocate(r, &r->args, 2 * sizeof(struct worker_arg)) ||
        allocate(r, &r->handles, 2 * sizeof(doca_dpa_dev_notification_completion_t))) return -1;
    struct worker_arg args[2] = {0};
    doca_dpa_dev_notification_completion_t handles[2] = {0};
    for (uint32_t i = 0; i < 2; ++i) {
        struct pinned *p = &r->pinned[i];
        args[i] = (struct worker_arg){.ctx = ctx, .event = event, .control = r->control.ptr,
            .x = r->data.ptr, .y = r->data.ptr + c->y_offset, .results = r->results.ptr,
            .reads = c->reads, .period = c->period,
            .timeout_cycles = (uint64_t)(c->timeout_ms * 1800000.0), .index = i};
        CHECK(doca_dpa_eu_affinity_create(r->dpa, &p->affinity));
        CHECK(doca_dpa_eu_affinity_set(p->affinity, c->eus[i]));
        unsigned int bound = UINT32_MAX;
        CHECK(doca_dpa_eu_affinity_get(p->affinity, &bound));
        if (bound != c->eus[i]) { fprintf(stderr, "Affinity ID mismatch\n"); return -1; }
        CHECK(doca_dpa_thread_create(r->dpa, &p->thread));
        CHECK(doca_dpa_thread_set_func_arg(p->thread, &bench_worker,
                                         r->args.ptr + i * sizeof(struct worker_arg)));
        CHECK(doca_dpa_thread_set_affinity(p->thread, p->affinity));
        CHECK(doca_dpa_thread_start(p->thread));
        CHECK(doca_dpa_notification_completion_create(r->dpa, p->thread, &p->notification));
        CHECK(doca_dpa_notification_completion_start(p->notification));
        CHECK(doca_dpa_notification_completion_get_dpa_handle(p->notification, &handles[i]));
        CHECK(doca_dpa_thread_run(p->thread));
    }
    CHECK(doca_dpa_h2d_memcpy(r->dpa, r->args.ptr, args, sizeof(args)));
    CHECK(doca_dpa_h2d_memcpy(r->dpa, r->handles.ptr, handles, sizeof(handles)));
    fprintf(stderr, "x=0x%" PRIx64 " y=0x%" PRIx64 " eus=%u,%u cores=%u,%u\n",
            (uint64_t)r->data.ptr, (uint64_t)r->data.ptr + c->y_offset,
            c->eus[0], c->eus[1], c->cores[0], c->cores[1]);
    return 0;
}

int runtime_sample(const struct config *c, struct runtime *r, struct result results[2])
{
    CHECK(doca_dpa_memset(r->dpa, r->control.ptr, 0, r->control.bytes));
    CHECK(doca_dpa_memset(r->dpa, r->data.ptr, 0, r->data.bytes));
    CHECK(doca_dpa_memset(r->dpa, r->results.ptr, 0, r->results.bytes));
    uint64_t x = X_VALUE;
    CHECK(doca_dpa_h2d_memcpy(r->dpa, r->data.ptr, &x, sizeof(x)));
    CHECK(doca_sync_event_update_set(r->event, 0));
    uint64_t retval = 0;
    doca_dpa_dev_t ctx;
    CHECK(doca_dpa_get_dpa_handle(r->dpa, &ctx));
    doca_error_t launch = doca_dpa_rpc(r->dpa, &bench_start, &retval, ctx, r->handles.ptr);
    if (launch != DOCA_SUCCESS) {
        r->failed = true;
        CHECK(launch);
    }
    const double end = seconds() + 5.0 + c->timeout_ms / 1000.0;
    for (;;) {
        uint64_t done = 0;
        CHECK(doca_sync_event_get(r->event, &done));
        if (done == 2) break;
        if (doca_dpa_peek_at_last_error(r->dpa) != DOCA_SUCCESS || seconds() >= end) {
            fprintf(stderr, "Host deadline or DPA runtime failure; completed=%" PRIu64 "\n", done);
            r->failed = true;
            return -1;
        }
        struct timespec delay = {.tv_nsec = 50000};
        nanosleep(&delay, NULL);
    }
    CHECK(doca_dpa_d2h_memcpy(r->dpa, results, r->results.ptr, r->results.bytes));
    return 0;
}

void runtime_close(struct runtime *r)
{
    if (r->failed && r->dpa) (void)doca_dpa_stop(r->dpa);
    for (uint32_t i = 0; i < 2; ++i) {
        struct pinned *p = &r->pinned[i];
        if (p->notification) {
            (void)doca_dpa_notification_completion_stop(p->notification);
            (void)doca_dpa_notification_completion_destroy(p->notification);
        }
        if (p->thread) {
            (void)doca_dpa_thread_stop(p->thread);
            (void)doca_dpa_thread_destroy(p->thread);
        }
        if (p->affinity) (void)doca_dpa_eu_affinity_destroy(p->affinity);
    }
    struct buffer *buffers[] = {&r->handles, &r->args, &r->results, &r->data, &r->control};
    for (unsigned i = 0; i < sizeof(buffers) / sizeof(buffers[0]); ++i)
        if (buffers[i]->raw) (void)doca_dpa_mem_free(r->dpa, buffers[i]->raw);
    if (r->event) {
        (void)doca_sync_event_stop(r->event);
        (void)doca_sync_event_destroy(r->event);
    }
    if (r->dpa) { if (!r->failed) (void)doca_dpa_stop(r->dpa); (void)doca_dpa_destroy(r->dpa); }
    if (r->dev) (void)doca_dev_close(r->dev);
}
