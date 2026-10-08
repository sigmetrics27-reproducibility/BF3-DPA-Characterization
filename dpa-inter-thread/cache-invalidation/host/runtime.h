#ifndef RUNTIME_H
#define RUNTIME_H
#include <stdbool.h>
#include <stddef.h>
#include <doca_dev.h>
#include <doca_dpa.h>
#include <doca_sync_event.h>
#include "bench.h"

struct config {
    const char *device, *topology;
    uint32_t eus[2], cores[2], slots[2], warmup, samples, y_offset;
    uint64_t reads, period;
    double timeout_ms;
};
struct buffer { doca_dpa_dev_uintptr_t raw, ptr; size_t bytes; };
struct pinned {
    struct doca_dpa_thread *thread;
    struct doca_dpa_eu_affinity *affinity;
    struct doca_dpa_notification_completion *notification;
};
struct runtime {
    struct doca_dev *dev;
    struct doca_dpa *dpa;
    struct doca_sync_event *event;
    struct pinned pinned[2];
    struct buffer control, data, results, args, handles;
    bool failed;
};
int runtime_open(const struct config *c, struct runtime *r);
int runtime_sample(const struct config *c, struct runtime *r, struct result results[2]);
void runtime_close(struct runtime *r);
#endif
