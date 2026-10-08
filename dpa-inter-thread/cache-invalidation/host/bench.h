#ifndef BENCH_H
#define BENCH_H
#include <stdint.h>

#define LINE_BYTES 64U
#define SLOT_BYTES 256U
#define X_VALUE UINT64_C(0x9e3779b97f4a7c15)

enum { STATUS_OK, STATUS_TIMEOUT, STATUS_BAD_RESULT };
struct slot { uint64_t value; uint8_t padding[SLOT_BYTES - 8]; };
struct control {
    struct slot ready[2], start, stop, abort, done[2];
};
struct result {
    uint64_t cycles, checksum, reads, writes, missed, first_write, last_write, final_y, status;
    uint8_t padding[SLOT_BYTES - 72];
};
struct worker_arg {
    uint64_t ctx, event, control, x, y, results, reads, period, timeout_cycles;
    uint32_t index;
};
_Static_assert(sizeof(struct slot) == SLOT_BYTES, "slot size");
_Static_assert(sizeof(struct result) == SLOT_BYTES, "result size");
#endif
