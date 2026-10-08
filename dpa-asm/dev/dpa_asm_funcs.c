#include <stdint.h>

// add a, b and c
uint64_t asm_sum3(uint64_t a, uint64_t b, uint64_t c)
{
    uint64_t r;
    __asm__ volatile("add %0,%1,%2\n\t"
                     "add %0,%0,%3"
                     : "=r"(r)
                     : "r"(a), "r"(b), "r"(c));
    return r;
}

// read the cycle counter with rdcycle
uint64_t asm_rdcycle(void)
{
    uint64_t c;
    __asm__ volatile("csrr %0, cycle"
                     : "=r"(c));
    return c;
}
