#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <random>
#include <utility>
#include <vector>

#include <doca_dpa.h>
#include <doca_log.h>

#include "host/dpa_offloading_host_utils.h"

extern "C" {
    doca_dpa_func_t dpa_offloading_kernel_compute_intensive;
    doca_dpa_func_t dpa_offloading_kernel_long_latency;
    doca_dpa_func_t dpa_offloading_kernel_pChasing_init_rpc;
}

DOCA_LOG_REGISTER(DPA_OFFLOADING_MODEL);

namespace {

volatile uint64_t g_host_workload_sink = 0;

uint32_t reduce_u32_add_host(volatile const uint32_t *src, uint32_t adds_per_element)
{
    uint32_t acc = 0;
    for (uint32_t i = 0; i < DPA_OFFLOADING_ELEMENTS; ++i) {
        const uint32_t elem = src[i];
        uint32_t repeat = 0;
        for (; repeat + 16 <= adds_per_element; repeat += 16) {
            acc += elem ^ (acc & 1U);
            acc += elem ^ (acc & 1U);
            acc += elem ^ (acc & 1U);
            acc += elem ^ (acc & 1U);
            acc += elem ^ (acc & 1U);
            acc += elem ^ (acc & 1U);
            acc += elem ^ (acc & 1U);
            acc += elem ^ (acc & 1U);
            acc += elem ^ (acc & 1U);
            acc += elem ^ (acc & 1U);
            acc += elem ^ (acc & 1U);
            acc += elem ^ (acc & 1U);
            acc += elem ^ (acc & 1U);
            acc += elem ^ (acc & 1U);
            acc += elem ^ (acc & 1U);
            acc += elem ^ (acc & 1U);
        }
        for (; repeat < adds_per_element; ++repeat) {
            acc += elem ^ (acc & 1U);
        }
    }
    return acc;
}

std::vector<dpa_offloading_ptr_chasing_node> make_host_pointer_chasing_loop()
{
    std::vector<dpa_offloading_ptr_chasing_node> nodes(DPA_OFFLOADING_PTR_CHASING_NODES);
    for (uint32_t i = 0; i < DPA_OFFLOADING_PTR_CHASING_NODES; ++i) {
        const uint32_t next = (i + 1U) % DPA_OFFLOADING_PTR_CHASING_NODES;
        nodes[i].next = reinterpret_cast<uint64_t>(&nodes[next]);
    }
    return nodes;
}

uint64_t pointer_chasing_host(const std::vector<dpa_offloading_ptr_chasing_node> &nodes,
                              uint32_t ptr_chasing_iter)
{
    if (nodes.empty()) {
        throw_runtime("Host pointer-chasing loop is empty");
    }

    uint64_t dummy = 0;
    volatile const dpa_offloading_ptr_chasing_node *node = nodes.data();
    for (uint32_t i = 0; i < ptr_chasing_iter; ++i) {
        node = reinterpret_cast<volatile const dpa_offloading_ptr_chasing_node *>(node->next);
        dummy += node->next;
    }
    return dummy;
}

uint64_t run_host_workload(const Config &cfg,
                           const Resources &r,
                           const std::vector<dpa_offloading_ptr_chasing_node> &host_ptr_chasing_nodes,
                           uint32_t work_iterations)
{
    if (cfg.kernel_kind == DPA_OFFLOADING_KERNEL_POINTER_CHASING) {
        return pointer_chasing_host(host_ptr_chasing_nodes, work_iterations);
    }
    return reduce_u32_add_host(r.host_region, work_iterations);
}

uint32_t expected_result(uint32_t seed, uint32_t adds_per_element)
{
    uint32_t input[DPA_OFFLOADING_ELEMENTS] = {};
    for (uint32_t i = 0; i < DPA_OFFLOADING_ELEMENTS; ++i) {
        input[i] = input_value(seed, i);
    }
    return reduce_u32_add_host(input, adds_per_element);
}

const char *kernel_name(uint32_t kernel_kind)
{
    return kernel_kind == DPA_OFFLOADING_KERNEL_POINTER_CHASING ? "pointer-chasing" : "compute";
}

std::vector<uint32_t> workload_values(const Config &cfg)
{
    if (cfg.kernel_kind == DPA_OFFLOADING_KERNEL_POINTER_CHASING) {
        return cfg.ptr_chasing_iters_values;
    }
    return cfg.adds_per_element_values;
}

void init_pointer_chasing_loop(Resources &r)
{
    if (r.ptr_chasing_base_dev_ptr == 0) {
        throw_runtime("Pointer-chasing DPA memory was not allocated");
    }

    uint64_t ret = 1;
    check_doca(doca_dpa_rpc(r.dpa,
                            &dpa_offloading_kernel_pChasing_init_rpc,
                            &ret,
                            r.dpa_handle,
                            static_cast<uint64_t>(r.ptr_chasing_base_dev_ptr),
                            static_cast<uint64_t>(DPA_OFFLOADING_PTR_CHASING_BYTES),
                            static_cast<uint64_t>(DPA_OFFLOADING_PTR_CHASING_STRIDE_BYTES)),
               "doca_dpa_rpc(dpa_offloading_kernel_pChasing_init_rpc)");
    if (ret != 0) {
        throw_runtime("dpa_offloading_kernel_pChasing_init_rpc returned non-zero status");
    }
}

void launch_offload(const Config &cfg, Resources &r, uint32_t work_iterations)
{
    r.seq++;

    if (cfg.kernel_kind == DPA_OFFLOADING_KERNEL_POINTER_CHASING) {
        check_doca(doca_dpa_kernel_launch_update_set(r.dpa,
                                                     nullptr,
                                                     0,
                                                     r.completion_event.event,
                                                     r.seq,
                                                     1,
                                                     &dpa_offloading_kernel_long_latency,
                                                     r.dpa_handle,
                                                     r.kernel_cycles_event.dpa_handle,
                                                     static_cast<uint64_t>(r.ptr_chasing_base_dev_ptr),
                                                     work_iterations),
                   "doca_dpa_kernel_launch_update_set(dpa_offloading_kernel_long_latency)");
        return;
    }

    check_doca(doca_dpa_kernel_launch_update_set(r.dpa,
                                                 nullptr,
                                                 0,
                                                 r.completion_event.event,
                                                 r.seq,
                                                 1,
                                                 &dpa_offloading_kernel_compute_intensive,
                                                 r.dpa_handle,
                                                 r.host_dpa_mmap_handle,
                                                 reinterpret_cast<uint64_t>(r.host_region),
                                                 r.kernel_cycles_event.dpa_handle,
                                                 work_iterations),
               "doca_dpa_kernel_launch_update_set(dpa_offloading_kernel_compute_intensive)");
}

OffloadStats measure_offload_once(const Config &cfg,
                                  Resources &r,
                                  uint32_t work_iterations,
                                  double overlap_delay_us,
                                  double overlap_delay_stddev_fraction,
                                  uint32_t expected)
{
    std::vector<double> wall_samples;
    std::vector<double> dpa_samples;
    wall_samples.reserve(cfg.iters);
    dpa_samples.reserve(cfg.iters);
    std::mt19937_64 rng(cfg.seed ^ (static_cast<uint64_t>(work_iterations) << 32) ^
                        (static_cast<uint64_t>(cfg.kernel_kind) << 48) ^
                        static_cast<uint64_t>(overlap_delay_us * 1000.0) ^
                        (static_cast<uint64_t>(overlap_delay_stddev_fraction * 1000000.0) << 8));
    const bool randomize_overlap_delay =
        cfg.random_overlap_delay && overlap_delay_us > 0.0 && overlap_delay_stddev_fraction > 0.0;
    std::normal_distribution<double> overlap_delay_dist;
    if (randomize_overlap_delay) {
        overlap_delay_dist =
            std::normal_distribution<double>(overlap_delay_us, overlap_delay_us * overlap_delay_stddev_fraction);
    }

    for (uint32_t iter = 0; iter < cfg.warmup + cfg.iters; ++iter) {
        if (cfg.kernel_kind == DPA_OFFLOADING_KERNEL_COMPUTE) {
            prepare_inputs(cfg, r);
        }
        double sampled_overlap_delay_us = overlap_delay_us;
        if (randomize_overlap_delay) {
            sampled_overlap_delay_us = overlap_delay_dist(rng);
            if (sampled_overlap_delay_us < 0.0) {
                sampled_overlap_delay_us = 0.0;
            }
        }

        const auto start = HostClock::now();
        launch_offload(cfg, r, work_iterations);
        if (sampled_overlap_delay_us > 0.0) {
            const auto delay = std::chrono::duration_cast<HostClock::duration>(
                std::chrono::duration<double, std::micro>(sampled_overlap_delay_us));
            const auto deadline = HostClock::now() + delay;
            while (HostClock::now() < deadline) {
            }
        }
        wait_for_offload_completion(r);
        const auto end = HostClock::now();

        const uint64_t kernel_cycles = read_kernel_cycles(r);
        if (cfg.verify && cfg.kernel_kind == DPA_OFFLOADING_KERNEL_COMPUTE && *r.host_result != expected) {
            throw_runtime("DPA offload verify failed");
        }

        if (iter >= cfg.warmup) {
            wall_samples.push_back(elapsed_us(start, end));
            dpa_samples.push_back(static_cast<double>(kernel_cycles) / static_cast<double>(cfg.clock_mhz));
        }
    }

    return {compute_stats(std::move(wall_samples)), compute_stats(std::move(dpa_samples))};
}

Stats measure_host_workload(const Config &cfg,
                            Resources &r,
                            const std::vector<dpa_offloading_ptr_chasing_node> &host_ptr_chasing_nodes,
                            uint32_t work_iterations,
                            uint32_t expected)
{
    std::vector<double> samples;
    samples.reserve(cfg.iters);

    for (uint32_t iter = 0; iter < cfg.warmup + cfg.iters; ++iter) {
        if (cfg.kernel_kind == DPA_OFFLOADING_KERNEL_COMPUTE) {
            prepare_inputs(cfg, r);
        }

        const auto start = HostClock::now();
        const uint64_t result = run_host_workload(cfg, r, host_ptr_chasing_nodes, work_iterations);
        const auto end = HostClock::now();
        g_host_workload_sink = result;

        if (cfg.verify && cfg.kernel_kind == DPA_OFFLOADING_KERNEL_COMPUTE &&
            static_cast<uint32_t>(result) != expected) {
            throw_runtime("Host workload verify failed");
        }

        if (iter >= cfg.warmup) {
            samples.push_back(elapsed_us(start, end));
        }
    }

    return compute_stats(std::move(samples));
}

void print_metadata(const Config &cfg)
{
    std::cout << "device=" << cfg.device_name
              << " mode=kernel_launch"
              << " kernel=" << kernel_name(cfg.kernel_kind)
              << " compute_elements=" << DPA_OFFLOADING_ELEMENTS
              << " compute_dtype=u32"
              << " compute_op=add"
              << " baseline=host_workload"
              << " efficiency_definition=modeled_total_us/offloaded_wall_avg_us"
              << " pointer_chasing_bytes=" << DPA_OFFLOADING_PTR_CHASING_BYTES
              << " pointer_chasing_stride_bytes=" << DPA_OFFLOADING_PTR_CHASING_STRIDE_BYTES
              << " adds_per_element_values=" << join_csv(cfg.adds_per_element_values)
              << " ptr_chasing_iters_values=" << join_csv(cfg.ptr_chasing_iters_values)
              << " random_overlap_delay=" << (cfg.random_overlap_delay ? 1 : 0)
              << " overlap_delay_distribution=" << (cfg.random_overlap_delay ? "normal" : "fixed")
              << " overlap_delay_stddev_fraction_values=" << join_csv(cfg.overlap_delay_stddev_fractions)
              << " warmup=" << cfg.warmup
              << " iters=" << cfg.iters
              << " clock_mhz=" << cfg.clock_mhz
              << " verify=" << (cfg.verify ? 1 : 0)
              << "\n";
}

void print_kernel_launch_header()
{
    std::cout
        << "offload_rate_percent,kernel,work_iterations,compute_elements,"
        << "pointer_chasing_bytes,pointer_chasing_stride_bytes,"
        << "host_baseline_avg_us,host_baseline_stddev_us,"
        << "host_baseline_min_us,host_baseline_p50_us,host_baseline_p95_us,"
        << "host_baseline_p99_us,host_baseline_max_us,"
        << "modeled_total_us,overlap_delay_us,overlap_delay_stddev_fraction,"
        << "offloaded_wall_avg_us,offloaded_wall_stddev_us,offloaded_wall_min_us,"
        << "offloaded_wall_p50_us,offloaded_wall_p95_us,offloaded_wall_p99_us,"
        << "offloaded_wall_max_us,dpa_kernel_avg_us,dpa_kernel_stddev_us,"
        << "dpa_kernel_min_us,dpa_kernel_p50_us,dpa_kernel_p95_us,dpa_kernel_p99_us,"
        << "dpa_kernel_max_us,offloading_efficiency_avg,offloading_efficiency_stddev,"
        << "result,status\n";
}

void print_kernel_launch_row(uint32_t rate,
                             const Config &cfg,
                             uint32_t work_iterations,
                             const Stats &host_baseline,
                             double modeled_total_us,
                             double overlap_delay_us,
                             double overlap_delay_stddev_fraction,
                             const OffloadStats &offload,
                             uint32_t result)
{
    const double efficiency_avg = modeled_total_us / offload.wall_us.avg;
    const double efficiency_stddev =
        modeled_total_us * offload.wall_us.stddev / (offload.wall_us.avg * offload.wall_us.avg);

    std::cout << rate
              << "," << kernel_name(cfg.kernel_kind)
              << "," << work_iterations
              << "," << DPA_OFFLOADING_ELEMENTS
              << "," << DPA_OFFLOADING_PTR_CHASING_BYTES
              << "," << DPA_OFFLOADING_PTR_CHASING_STRIDE_BYTES
              << "," << std::fixed << std::setprecision(6) << host_baseline.avg
              << "," << host_baseline.stddev
              << "," << host_baseline.min
              << "," << host_baseline.p50
              << "," << host_baseline.p95
              << "," << host_baseline.p99
              << "," << host_baseline.max
              << "," << modeled_total_us
              << "," << overlap_delay_us
              << "," << overlap_delay_stddev_fraction
              << "," << offload.wall_us.avg
              << "," << offload.wall_us.stddev
              << "," << offload.wall_us.min
              << "," << offload.wall_us.p50
              << "," << offload.wall_us.p95
              << "," << offload.wall_us.p99
              << "," << offload.wall_us.max
              << "," << offload.dpa_kernel_us.avg
              << "," << offload.dpa_kernel_us.stddev
              << "," << offload.dpa_kernel_us.min
              << "," << offload.dpa_kernel_us.p50
              << "," << offload.dpa_kernel_us.p95
              << "," << offload.dpa_kernel_us.p99
              << "," << offload.dpa_kernel_us.max
              << "," << efficiency_avg
              << "," << efficiency_stddev
              << "," << result
              << ",ok\n";
}

void run_benchmark(const Config &cfg)
{
    Resources res{};
    try {
        init_resources(cfg, res);
        std::vector<dpa_offloading_ptr_chasing_node> host_ptr_chasing_nodes;
        if (cfg.kernel_kind == DPA_OFFLOADING_KERNEL_POINTER_CHASING) {
            init_pointer_chasing_loop(res);
            host_ptr_chasing_nodes = make_host_pointer_chasing_loop();
        } else {
            prepare_inputs(cfg, res);
        }

        print_metadata(cfg);
        if (cfg.print_header) {
            print_kernel_launch_header();
        }

        for (uint32_t work_iterations : workload_values(cfg)) {
            const uint32_t expected =
                cfg.kernel_kind == DPA_OFFLOADING_KERNEL_COMPUTE ? expected_result(cfg.seed, work_iterations) : 0U;
            const Stats host_baseline_stats =
                measure_host_workload(cfg, res, host_ptr_chasing_nodes, work_iterations, expected);
            if (host_baseline_stats.avg <= 0.0) {
                throw_runtime("Measured host baseline time is zero");
            }

            for (double overlap_delay_stddev_fraction : cfg.overlap_delay_stddev_fractions) {
                for (uint32_t rate : cfg.offload_rates) {
                    const double rate_fraction = static_cast<double>(rate) / 100.0;
                    const double modeled_total_us = host_baseline_stats.avg / rate_fraction;
                    const double overlap_delay_us = modeled_total_us - host_baseline_stats.avg;
                    const OffloadStats offload_stats = measure_offload_once(cfg,
                                                                            res,
                                                                            work_iterations,
                                                                            overlap_delay_us,
                                                                            overlap_delay_stddev_fraction,
                                                                            expected);
                    print_kernel_launch_row(rate,
                                            cfg,
                                            work_iterations,
                                            host_baseline_stats,
                                            modeled_total_us,
                                            overlap_delay_us,
                                            overlap_delay_stddev_fraction,
                                            offload_stats,
                                            expected);
                }
            }
        }

        cleanup_resources(res);
    } catch (...) {
        cleanup_resources(res);
        throw;
    }
}

} // namespace

int main(int argc, char **argv)
{
    try {
        init_doca_log();
        const Config cfg = parse_args(argc, argv);
        run_benchmark(cfg);
        return EXIT_SUCCESS;
    } catch (const std::exception &e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return EXIT_FAILURE;
    }
}
