#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DPA_TPUT_SAMECORE_MAX_THREADS 16U

struct dpa_tput_samecore_thread_result {
	uint64_t start_cycle;
	uint64_t end_cycle;
	uint64_t cycles;
	uint64_t instret;
	uint64_t checksum;
};

struct dpa_tput_samecore_thread_arg {
	uint64_t dpa_ctx;
	uint64_t done_se;
	uint64_t ready_se;
	uint64_t results_dev_ptr;
	uint64_t start_flag_dev_ptr;
	uint64_t op_iterations;
	uint32_t thread_index;
	uint32_t reserved;
};

#ifdef __cplusplus
}
#endif
