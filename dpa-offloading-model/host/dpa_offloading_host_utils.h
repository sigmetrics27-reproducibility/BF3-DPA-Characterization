#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <doca_dev.h>
#include <doca_dpa.h>
#include <doca_error.h>
#include <doca_mmap.h>
#include <doca_sync_event.h>

#include "dpa_offloading_common.h"

using HostClock = std::chrono::steady_clock;

struct Config {
    std::string device_name = "mlx5_0";
    uint32_t kernel_kind = DPA_OFFLOADING_KERNEL_COMPUTE;
    std::vector<uint32_t> offload_rates = {10, 20, 30, 40, 50, 60, 70, 80, 90};
    std::vector<uint32_t> adds_per_element_values = {16, 32, 48, 64};
    std::vector<uint32_t> ptr_chasing_iters_values = {1024, 4096, 16384};
    uint32_t warmup = 20;
    uint32_t iters = 200;
    uint32_t clock_mhz = 1800;
    uint32_t seed = 1;
    bool verify = false;
    bool print_header = true;
    bool random_overlap_delay = false;
    std::vector<double> overlap_delay_stddev_fractions = {0.1};
};

struct SyncEventResource {
    doca_sync_event *event = nullptr;
    doca_dpa_dev_sync_event_t dpa_handle = 0;
};

struct Resources {
    doca_dev *dev = nullptr;
    doca_dpa *dpa = nullptr;
    doca_dpa_dev_t dpa_handle = 0;

    void *host_mem_base = nullptr;
    size_t host_mem_bytes = 0;
    uint32_t *host_region = nullptr;
    uint32_t *host_result = nullptr;
    doca_mmap *host_mmap = nullptr;
    doca_dpa_dev_mmap_t host_dpa_mmap_handle = 0;

    SyncEventResource completion_event;
    SyncEventResource kernel_cycles_event;
    uint64_t seq = 0;

    doca_dpa_dev_uintptr_t ptr_chasing_alloc_dev_ptr = 0;
    doca_dpa_dev_uintptr_t ptr_chasing_base_dev_ptr = 0;
    size_t ptr_chasing_alloc_bytes = 0;
};

struct Stats {
    double avg = 0.0;
    double stddev = 0.0;
    double min = 0.0;
    double p50 = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
    double max = 0.0;
};

struct OffloadStats {
    Stats wall_us;
    Stats dpa_kernel_us;
};

[[noreturn]] void throw_runtime(const std::string &msg);
void check_doca(doca_error_t status, const char *what);
double elapsed_us(HostClock::time_point start, HostClock::time_point end);

void open_device_by_ibdev_name(const std::string &device_name, doca_dev **dev);
void init_doca_log();

Config parse_args(int argc, char **argv);
void print_usage(const char *prog);

Stats compute_stats(std::vector<double> samples);
std::string join_csv(const std::vector<uint32_t> &values);
std::string join_csv(const std::vector<double> &values);
uint32_t input_value(uint32_t seed, uint32_t i);
void prepare_inputs(const Config &cfg, Resources &r);
void busy_wait_for_us(double delay_us);
uint64_t us_to_cycles(double delay_us, uint32_t clock_mhz);

void init_resources(const Config &cfg, Resources &r);
void cleanup_resources(Resources &r);
void wait_for_offload_completion(Resources &r);
uint64_t read_kernel_cycles(Resources &r);

void print_header();
void print_row(uint32_t rate,
               uint32_t adds_per_element,
               double workload_delay_us,
               const Stats &host,
               double theoretical_host_total_us,
               double overlap_delay_us,
               const OffloadStats &offload,
               uint32_t result);
