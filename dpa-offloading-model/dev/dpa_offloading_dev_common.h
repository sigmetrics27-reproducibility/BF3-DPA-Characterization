#pragma once

#include <stdint.h>

#include <doca_dpa_dev.h>
#include <doca_dpa_dev_buf.h>
#include <doca_dpa_dev_sync_event.h>
#include <dpaintrin.h>

#include "../host/dpa_offloading_common.h"

static __always_inline uint64_t dpa_offloading_elapsed_cycles(uint64_t start, uint64_t end)
{
    if (end >= start) {
        return end - start;
    }

    return ((1ULL << 32) - 1ULL) - start + end;
}
