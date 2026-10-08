#include <doca_dpa_dev.h>
#include <doca_dpa_dev_sync_event.h>

#include "dpa_asm_demo_common.h"

// asm functions from dpaasm
extern uint64_t asm_sum3(uint64_t a, uint64_t b, uint64_t c);
extern uint64_t asm_rdcycle(void);

__dpa_global__ void k_demo(uint64_t arg)
{
    struct dpa_asm_demo_cfg *c = (struct dpa_asm_demo_cfg *)(uintptr_t)arg;
    if (c == NULL) {
        return;
    }

    const uint64_t sum = asm_sum3(c->a, c->b, c->c);
    const uint64_t cyc = asm_rdcycle();

    doca_dpa_dev_sync_event_update_set(c->se_sum, sum);
    doca_dpa_dev_sync_event_update_set(c->se_cyc, cyc);
}
