#include <doca_dpa_dev.h>
#include <doca_dpa_dev_sync_event.h>
#include <dpaintrin.h>

#include "dpa_compute_tput_samecore_common.h"

#define START_FLAG_RELEASE_DELAY_CYCLES 1000000ULL

#define ASM_PROTO(name) extern uint64_t name(uint64_t)
ASM_PROTO(bench_sc_u64_add); ASM_PROTO(bench_sc_u64_mul); ASM_PROTO(bench_sc_u64_div);
ASM_PROTO(bench_sc_u64_xor); ASM_PROTO(bench_sc_u64_and); ASM_PROTO(bench_sc_u64_min); ASM_PROTO(bench_sc_u64_max);
ASM_PROTO(bench_sc_u32_add); ASM_PROTO(bench_sc_u32_mul); ASM_PROTO(bench_sc_u32_div);
ASM_PROTO(bench_sc_u32_xor); ASM_PROTO(bench_sc_u32_and); ASM_PROTO(bench_sc_u32_min); ASM_PROTO(bench_sc_u32_max);
ASM_PROTO(bench_sc_u8_add);  ASM_PROTO(bench_sc_u8_mul);  ASM_PROTO(bench_sc_u8_div);
ASM_PROTO(bench_sc_u8_xor);  ASM_PROTO(bench_sc_u8_and);  ASM_PROTO(bench_sc_u8_min);  ASM_PROTO(bench_sc_u8_max);
#undef ASM_PROTO

#define WORKER(kname, afn) \
	__dpa_global__ void kname(uint64_t arg) \
	{ \
		(void)arg; \
		struct dpa_tput_samecore_thread_arg *thread_arg = \
			(struct dpa_tput_samecore_thread_arg *)doca_dpa_dev_thread_get_local_storage(); \
		struct dpa_tput_samecore_thread_result *results = \
			(struct dpa_tput_samecore_thread_result *)thread_arg->results_dev_ptr; \
		if (thread_arg->dpa_ctx != 0) { \
			doca_dpa_dev_device_set(thread_arg->dpa_ctx); \
		} \
		uint64_t *start_flag = (uint64_t *)thread_arg->start_flag_dev_ptr; \
		if (thread_arg->ready_se != 0) { \
			doca_dpa_dev_sync_event_update_add((doca_dpa_dev_sync_event_t)thread_arg->ready_se, 1); \
		} \
		uint64_t start_state; \
		do { \
			start_state = __atomic_load_n(start_flag, __ATOMIC_ACQUIRE); \
		} while (start_state == 0); \
		if (start_state != 1) { \
			doca_dpa_dev_sync_event_update_add((doca_dpa_dev_sync_event_t)thread_arg->done_se, 1); \
			doca_dpa_dev_thread_finish(); \
			return; \
		} \
		const uint64_t c0 = __dpa_thread_cycles(); \
		const uint64_t i0 = __dpa_thread_inst_ret(); \
		const uint64_t checksum = afn(thread_arg->op_iterations); \
		const uint64_t c1 = __dpa_thread_cycles(); \
		const uint64_t i1 = __dpa_thread_inst_ret(); \
		results[thread_arg->thread_index].start_cycle = c0; \
		results[thread_arg->thread_index].end_cycle = c1; \
		results[thread_arg->thread_index].cycles = c1 - c0; \
		results[thread_arg->thread_index].instret = i1 - i0; \
		results[thread_arg->thread_index].checksum = checksum; \
		__dpa_thread_memory_writeback(); \
		doca_dpa_dev_sync_event_update_add((doca_dpa_dev_sync_event_t)thread_arg->done_se, 1); \
		doca_dpa_dev_thread_finish(); \
	}

WORKER(k_tput_samecore_u64_add, bench_sc_u64_add)
WORKER(k_tput_samecore_u64_mul, bench_sc_u64_mul)
WORKER(k_tput_samecore_u64_div, bench_sc_u64_div)
WORKER(k_tput_multicore_u64_div, bench_sc_u64_div)
WORKER(k_tput_samecore_u64_xor, bench_sc_u64_xor)
WORKER(k_tput_samecore_u64_and, bench_sc_u64_and)
WORKER(k_tput_samecore_u64_min, bench_sc_u64_min)
WORKER(k_tput_samecore_u64_max, bench_sc_u64_max)
WORKER(k_tput_samecore_u32_add, bench_sc_u32_add)
WORKER(k_tput_samecore_u32_mul, bench_sc_u32_mul)
WORKER(k_tput_samecore_u32_div, bench_sc_u32_div)
WORKER(k_tput_samecore_u32_xor, bench_sc_u32_xor)
WORKER(k_tput_samecore_u32_and, bench_sc_u32_and)
WORKER(k_tput_samecore_u32_min, bench_sc_u32_min)
WORKER(k_tput_samecore_u32_max, bench_sc_u32_max)
WORKER(k_tput_samecore_u8_add, bench_sc_u8_add)
WORKER(k_tput_samecore_u8_mul, bench_sc_u8_mul)
WORKER(k_tput_samecore_u8_div, bench_sc_u8_div)
WORKER(k_tput_samecore_u8_xor, bench_sc_u8_xor)
WORKER(k_tput_samecore_u8_and, bench_sc_u8_and)
WORKER(k_tput_samecore_u8_min, bench_sc_u8_min)
WORKER(k_tput_samecore_u8_max, bench_sc_u8_max)

#undef WORKER

static void wait_cycles(uint64_t cycles)
{
	const uint64_t start = __dpa_thread_cycles();

	while ((__dpa_thread_cycles() - start) < cycles) {
	}
}

__dpa_rpc__ uint64_t k_tput_samecore_start(doca_dpa_dev_uintptr_t notify_handles_dev_ptr,
					   uint64_t thread_count,
					   doca_dpa_dev_uintptr_t start_flag_dev_ptr)
{
	doca_dpa_dev_notification_completion_t *notify_handles =
		(doca_dpa_dev_notification_completion_t *)notify_handles_dev_ptr;
	uint64_t *start_flag = (uint64_t *)start_flag_dev_ptr;

	for (uint64_t i = 0; i < thread_count; ++i) {
		doca_dpa_dev_thread_notify(notify_handles[i]);
	}

	wait_cycles(START_FLAG_RELEASE_DELAY_CYCLES);
	__atomic_store_n(start_flag, 1, __ATOMIC_RELEASE);
	__dpa_thread_memory_writeback();

	return 0;
}

__dpa_rpc__ uint64_t k_tput_multicore_start(doca_dpa_dev_uintptr_t notify_handles_dev_ptr,
					   uint64_t thread_count,
					   doca_dpa_dev_uintptr_t start_flag_dev_ptr,
					   doca_dpa_dev_sync_event_t ready_se,
					   uint64_t ready_target)
{
	doca_dpa_dev_notification_completion_t *notify_handles =
		(doca_dpa_dev_notification_completion_t *)notify_handles_dev_ptr;
	for (uint64_t i = 0; i < thread_count; ++i) {
		doca_dpa_dev_thread_notify(notify_handles[i]);
	}
	wait_cycles(START_FLAG_RELEASE_DELAY_CYCLES);
	uint64_t ready_count = 0;
	doca_dpa_dev_sync_event_get(ready_se, &ready_count);
	uint64_t *start_flag = (uint64_t *)start_flag_dev_ptr;
	// 1 starts timing, 2 exits if the ready check fails
	__atomic_store_n(start_flag, ready_count == ready_target ? 1 : 2, __ATOMIC_RELEASE);
	__dpa_thread_memory_writeback();
	return ready_count;
}
