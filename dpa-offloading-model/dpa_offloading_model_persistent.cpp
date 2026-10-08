
#include <algorithm>
#include <atomic>
#include <chrono>
#include <xmmintrin.h>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <doca_dev.h>
#include <doca_dpa.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_mmap.h>
#include <doca_sync_event.h>
#include <unistd.h>

#include "host/dpa_offloading_common.h"
#include "host/dpa_offloading_host_utils.h"

extern struct doca_dpa_app *DPA_APP_NAME;

extern "C" {
doca_dpa_func_t dpa_offloading_persistent_thread;
doca_dpa_func_t dpa_offloading_persistent_kickstart_rpc;
doca_dpa_func_t dpa_offloading_kernel_pChasing_init_rpc;
}

DOCA_LOG_REGISTER(DPA_OFFLOADING_PERSISTENT);

namespace {

constexpr size_t kShmAlignment = 4096;
constexpr size_t kExternalPtrLineBytes = 64;

uint64_t align_up(uint64_t value, uint64_t align)
{
    const uint64_t rem = value % align;
    return (rem == 0) ? value : value + (align - rem);
}

struct PersistentConfig {
    std::string device_name = "mlx5_0";
    uint32_t kernel_kind = DPA_OFFLOADING_KERNEL_COMPUTE;
    std::vector<uint32_t> offload_rates = {10, 20, 30, 40, 50, 60, 70, 80, 90};
    std::vector<uint32_t> adds_per_element_values = {16, 32, 48, 64};
    std::vector<uint32_t> ptr_chasing_iters_values = {1024, 4096, 16384};
    std::vector<double> overlap_delay_stddev_fractions = {0.1};
    uint32_t warmup = 20;
    uint32_t iters = 200;
    uint32_t clock_mhz = 1800;
    uint32_t seed = 1;
    bool verify = false;
    bool print_header = true;
    bool random_overlap_delay = false;
    bool affinity_on = false;
    uint32_t eu_id = 0;
};

struct PersistentResources {
    doca_dev *dev = nullptr;
    doca_dpa *dpa = nullptr;
    doca_dpa_dev_t dpa_handle = 0;

    void *shm_base = nullptr;
    size_t shm_bytes = 0;
    struct dpa_offloading_persistent_shm *shm = nullptr;
    doca_mmap *shm_mmap = nullptr;
    doca_dpa_dev_mmap_t shm_dpa_handle = 0;

    doca_dpa_thread *thread = nullptr;
    doca_dpa_eu_affinity *affinity = nullptr;
    doca_dpa_notification_completion *notify_comp = nullptr;
    doca_dpa_dev_notification_completion_t notify_handle = 0;
    doca_dpa_dev_uintptr_t thread_arg_dev = 0;

    doca_dpa_dev_uintptr_t ptr_chasing_alloc_dev_ptr = 0;
    doca_dpa_dev_uintptr_t ptr_chasing_base_dev_ptr = 0;
    size_t ptr_chasing_alloc_bytes = 0;

    uint64_t request_seq = 0;
};

uint64_t parse_u64(const std::string &s, const char *name)
{
    size_t idx = 0;
    uint64_t v = 0;
    try {
        v = std::stoull(s, &idx, 0);
    } catch (const std::exception &) {
        throw_runtime(std::string("Invalid ") + name + ": " + s);
    }
    if (idx != s.size()) {
        throw_runtime(std::string("Invalid ") + name + ": " + s);
    }
    return v;
}

double parse_double(const std::string &s, const char *name)
{
    size_t idx = 0;
    double v = 0.0;
    try {
        v = std::stod(s, &idx);
    } catch (const std::exception &) {
        throw_runtime(std::string("Invalid ") + name + ": " + s);
    }
    if (idx != s.size() || !std::isfinite(v)) {
        throw_runtime(std::string("Invalid ") + name + ": " + s);
    }
    return v;
}

std::vector<uint32_t> parse_uint_csv(const std::string &csv, const char *name)
{
    std::vector<uint32_t> out;
    std::stringstream ss(csv);
    std::string token;
    while (std::getline(ss, token, ',')) {
        if (token.empty()) {
            continue;
        }
        out.push_back(static_cast<uint32_t>(parse_u64(token, name)));
    }
    if (out.empty()) {
        throw_runtime(std::string(name) + " must not be empty");
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<double> parse_double_csv(const std::string &csv, const char *name)
{
    std::vector<double> out;
    std::stringstream ss(csv);
    std::string token;
    while (std::getline(ss, token, ',')) {
        if (token.empty()) {
            continue;
        }
        out.push_back(parse_double(token, name));
    }
    if (out.empty()) {
        throw_runtime(std::string(name) + " must not be empty");
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

void print_usage(const char *prog)
{
    std::cerr
        << "Usage:\n"
        << "  " << prog << " --device mlx5_0 [options]\n\n"
        << "Persistent offload-thread variant. Options:\n"
        << "  --device <name>              Device name (default: mlx5_0)\n"
        << "  --kernel <name>              Workload kernel: compute or pointer-chasing (default: compute)\n"
        << "  --offload-rates <csv>        Percent rates in [1,100] (default: 10,20,...,90)\n"
        << "  --adds-per-element <csv>     Add counts, positive uint32 values (default: 16,32,48,64)\n"
        << "  --ptr-chasing-iters <csv>    Pointer-chasing iteration counts (default: 1024,4096,16384)\n"
        << "  --warmup <num>               Warmup measurements per phase (default: 20)\n"
        << "  --iters <num>                Measurements per phase (default: 200)\n"
        << "  --clock-mhz <num>            DPA clock MHz for cycle conversion (default: 1800)\n"
        << "  --seed <num>                 Seed for deterministic input pattern (default: 1)\n"
        << "  --random-overlap-delay <on|off>\n"
        << "                               Sample overlap delay from a normal distribution (default: off)\n"
        << "  --overlap-delay-stddev-fraction <csv>\n"
        << "                               Random delay stddev / mean fractions (default: 0.1)\n"
        << "  --affinity <on|off>          Pin the offload thread to one EU (default: off)\n"
        << "  --eu-id <num>                EU id for --affinity (default: 0)\n"
        << "  --verify                     Verify host and DPA scalar results\n"
        << "  --no-header                  Suppress CSV header line\n";
}

PersistentConfig parse_persistent_args(int argc, char **argv)
{
    PersistentConfig cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&](const char *name) -> std::string {
            if (i + 1 >= argc) {
                throw_runtime(std::string("Missing value for ") + name);
            }
            return std::string(argv[++i]);
        };

        if (arg == "--device" || arg == "--device-name") {
            cfg.device_name = next(arg.c_str());
        } else if (arg == "--kernel") {
            const std::string v = next("--kernel");
            if (v == "compute") {
                cfg.kernel_kind = DPA_OFFLOADING_KERNEL_COMPUTE;
            } else if (v == "pointer-chasing" || v == "pointer_chasing" || v == "pointer") {
                cfg.kernel_kind = DPA_OFFLOADING_KERNEL_POINTER_CHASING;
            } else {
                throw_runtime("--kernel must be 'compute' or 'pointer-chasing'");
            }
        } else if (arg == "--offload-rates") {
            cfg.offload_rates = parse_uint_csv(next("--offload-rates"), "--offload-rates");
        } else if (arg == "--adds-per-element") {
            cfg.adds_per_element_values = parse_uint_csv(next("--adds-per-element"), "--adds-per-element");
        } else if (arg == "--ptr-chasing-iters") {
            cfg.ptr_chasing_iters_values = parse_uint_csv(next("--ptr-chasing-iters"), "--ptr-chasing-iters");
        } else if (arg == "--warmup") {
            cfg.warmup = static_cast<uint32_t>(parse_u64(next("--warmup"), "--warmup"));
        } else if (arg == "--iters") {
            cfg.iters = static_cast<uint32_t>(parse_u64(next("--iters"), "--iters"));
        } else if (arg == "--clock-mhz") {
            cfg.clock_mhz = static_cast<uint32_t>(parse_u64(next("--clock-mhz"), "--clock-mhz"));
        } else if (arg == "--seed") {
            cfg.seed = static_cast<uint32_t>(parse_u64(next("--seed"), "--seed"));
        } else if (arg == "--random-overlap-delay") {
            const std::string v = next("--random-overlap-delay");
            if (v == "on") {
                cfg.random_overlap_delay = true;
            } else if (v == "off") {
                cfg.random_overlap_delay = false;
            } else {
                throw_runtime("--random-overlap-delay must be 'on' or 'off'");
            }
        } else if (arg == "--overlap-delay-stddev-fraction") {
            cfg.overlap_delay_stddev_fractions =
                parse_double_csv(next("--overlap-delay-stddev-fraction"), "--overlap-delay-stddev-fraction");
        } else if (arg == "--affinity") {
            const std::string v = next("--affinity");
            if (v == "on") {
                cfg.affinity_on = true;
            } else if (v == "off") {
                cfg.affinity_on = false;
            } else {
                throw_runtime("--affinity must be 'on' or 'off'");
            }
        } else if (arg == "--eu-id") {
            cfg.eu_id = static_cast<uint32_t>(parse_u64(next("--eu-id"), "--eu-id"));
        } else if (arg == "--verify") {
            cfg.verify = true;
        } else if (arg == "--no-header") {
            cfg.print_header = false;
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            std::exit(EXIT_SUCCESS);
        } else {
            throw_runtime("Unknown argument: " + arg);
        }
    }

    for (uint32_t rate : cfg.offload_rates) {
        if (rate == 0 || rate > 100) {
            throw_runtime("--offload-rates entries must be in [1, 100]");
        }
    }
    for (uint32_t value : cfg.adds_per_element_values) {
        if (value == 0) {
            throw_runtime("--adds-per-element entries must be positive uint32 values");
        }
    }
    for (uint32_t value : cfg.ptr_chasing_iters_values) {
        if (value == 0) {
            throw_runtime("--ptr-chasing-iters entries must be positive uint32 values");
        }
    }
    for (double value : cfg.overlap_delay_stddev_fractions) {
        if (value < 0.0) {
            throw_runtime("--overlap-delay-stddev-fraction entries must be >= 0");
        }
    }
    if (cfg.iters == 0) {
        throw_runtime("--iters must be > 0");
    }
    if (cfg.clock_mhz == 0) {
        throw_runtime("--clock-mhz must be > 0");
    }
    return cfg;
}

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

const char *kernel_name(uint32_t kernel_kind)
{
    return kernel_kind == DPA_OFFLOADING_KERNEL_POINTER_CHASING ? "pointer-chasing" : "compute";
}

std::vector<uint32_t> workload_values(const PersistentConfig &cfg)
{
    if (cfg.kernel_kind == DPA_OFFLOADING_KERNEL_POINTER_CHASING) {
        return cfg.ptr_chasing_iters_values;
    }
    return cfg.adds_per_element_values;
}

uint64_t run_host_workload(const PersistentConfig &cfg,
                           PersistentResources &r,
                           const std::vector<dpa_offloading_ptr_chasing_node> &host_ptr_chasing_nodes,
                           uint32_t work_iterations)
{
    if (cfg.kernel_kind == DPA_OFFLOADING_KERNEL_POINTER_CHASING) {
        return pointer_chasing_host(host_ptr_chasing_nodes, work_iterations);
    }
    return reduce_u32_add_host(r.shm->command.inputs, work_iterations);
}

uint32_t expected_result(uint32_t seed, uint32_t adds_per_element)
{
    uint32_t input[DPA_OFFLOADING_ELEMENTS] = {};
    for (uint32_t i = 0; i < DPA_OFFLOADING_ELEMENTS; ++i) {
        input[i] = input_value(seed, i);
    }
    return reduce_u32_add_host(input, adds_per_element);
}

void prepare_inputs(const PersistentConfig &cfg, PersistentResources &r)
{
    for (uint32_t i = 0; i < DPA_OFFLOADING_ELEMENTS; ++i) {
        r.shm->command.inputs[i] = input_value(cfg.seed, i);
    }
}

Stats measure_host_workload(const PersistentConfig &cfg,
                            PersistentResources &r,
                            const std::vector<dpa_offloading_ptr_chasing_node> &host_ptr_chasing_nodes,
                            uint32_t work_iterations,
                            uint32_t expected)
{
    std::vector<double> samples;
    samples.reserve(cfg.iters);

    volatile uint64_t sink = 0;
    for (uint32_t iter = 0; iter < cfg.warmup + cfg.iters; ++iter) {
        if (cfg.kernel_kind == DPA_OFFLOADING_KERNEL_COMPUTE) {
            prepare_inputs(cfg, r);
        }

        const auto start = HostClock::now();
        const uint64_t result = run_host_workload(cfg, r, host_ptr_chasing_nodes, work_iterations);
        const auto end = HostClock::now();
        sink ^= result;

        if (cfg.verify && cfg.kernel_kind == DPA_OFFLOADING_KERNEL_COMPUTE &&
            static_cast<uint32_t>(result) != expected) {
            throw_runtime("Host workload verify failed");
        }

        if (iter >= cfg.warmup) {
            samples.push_back(elapsed_us(start, end));
        }
    }

    if (sink == std::numeric_limits<uint64_t>::max()) {
        std::cerr << "";
    }
    return compute_stats(std::move(samples));
}

// persistent thread lifecycle

void cleanup_persistent(PersistentResources &r)
{
    // give the worker time to see the stop flag and finish before cleanup
    if (r.shm != nullptr && r.thread != nullptr) {
        r.shm->command.shutdown_flag = 1;
        std::atomic_thread_fence(std::memory_order_release);
        const char *fb = reinterpret_cast<const char *>(&r.shm->command);
        for (size_t off = 0; off < sizeof(r.shm->command); off += 64) {
            _mm_clflush(fb + off);
        }
        _mm_mfence();
        usleep(20 * 1000);
    }

    if (r.notify_comp != nullptr) {
        (void)doca_dpa_notification_completion_stop(r.notify_comp);
        (void)doca_dpa_notification_completion_destroy(r.notify_comp);
        r.notify_comp = nullptr;
        r.notify_handle = 0;
    }
    if (r.thread != nullptr) {
        (void)doca_dpa_thread_stop(r.thread);
        (void)doca_dpa_thread_destroy(r.thread);
        r.thread = nullptr;
    }
    if (r.affinity != nullptr) {
        (void)doca_dpa_eu_affinity_destroy(r.affinity);
        r.affinity = nullptr;
    }
    if (r.thread_arg_dev != 0 && r.dpa != nullptr) {
        (void)doca_dpa_mem_free(r.dpa, r.thread_arg_dev);
        r.thread_arg_dev = 0;
    }
    if (r.ptr_chasing_alloc_dev_ptr != 0 && r.dpa != nullptr) {
        (void)doca_dpa_mem_free(r.dpa, r.ptr_chasing_alloc_dev_ptr);
        r.ptr_chasing_alloc_dev_ptr = 0;
        r.ptr_chasing_base_dev_ptr = 0;
        r.ptr_chasing_alloc_bytes = 0;
    }

    if (r.shm_mmap != nullptr) {
        (void)doca_mmap_destroy(r.shm_mmap);
        r.shm_mmap = nullptr;
        r.shm_dpa_handle = 0;
    }
    if (r.shm_base != nullptr) {
        free(r.shm_base);
        r.shm_base = nullptr;
        r.shm = nullptr;
    }
    if (r.dpa != nullptr) {
        (void)doca_dpa_stop(r.dpa);
        (void)doca_dpa_destroy(r.dpa);
        r.dpa = nullptr;
    }
    if (r.dev != nullptr) {
        (void)doca_dev_close(r.dev);
        r.dev = nullptr;
    }
}

void init_persistent(const PersistentConfig &cfg, PersistentResources &r)
{
    open_device_by_ibdev_name(cfg.device_name, &r.dev);
    check_doca(doca_dpa_create(r.dev, &r.dpa), "doca_dpa_create");
    check_doca(doca_dpa_set_app(r.dpa, DPA_APP_NAME), "doca_dpa_set_app");
    check_doca(doca_dpa_start(r.dpa), "doca_dpa_start");
    check_doca(doca_dpa_get_dpa_handle(r.dpa, &r.dpa_handle), "doca_dpa_get_dpa_handle");

    // memory shared by the host and DPA
    r.shm_bytes = static_cast<size_t>(align_up(sizeof(struct dpa_offloading_persistent_shm), kExternalPtrLineBytes));
    if (posix_memalign(&r.shm_base, kShmAlignment, r.shm_bytes) != 0 || r.shm_base == nullptr) {
        throw_runtime("posix_memalign failed for persistent shm");
    }
    std::memset(r.shm_base, 0, r.shm_bytes);
    r.shm = static_cast<struct dpa_offloading_persistent_shm *>(r.shm_base);

    check_doca(doca_mmap_create(&r.shm_mmap), "doca_mmap_create(shm)");
    try {
        check_doca(doca_mmap_set_permissions(r.shm_mmap, DOCA_ACCESS_FLAG_LOCAL_READ_WRITE),
                   "doca_mmap_set_permissions(shm)");
        check_doca(doca_mmap_set_memrange(r.shm_mmap, r.shm_base, r.shm_bytes),
                   "doca_mmap_set_memrange(shm)");
        check_doca(doca_mmap_add_dev(r.shm_mmap, r.dev), "doca_mmap_add_dev(shm)");
        check_doca(doca_mmap_start(r.shm_mmap), "doca_mmap_start(shm)");
        check_doca(doca_mmap_dev_get_dpa_handle(r.shm_mmap, r.dev, &r.shm_dpa_handle),
                   "doca_mmap_dev_get_dpa_handle(shm)");
    } catch (...) {
        cleanup_persistent(r);
        throw;
    }

    if (cfg.kernel_kind == DPA_OFFLOADING_KERNEL_POINTER_CHASING) {
        r.ptr_chasing_alloc_bytes =
            DPA_OFFLOADING_PTR_CHASING_BYTES + DPA_OFFLOADING_PTR_CHASING_STRIDE_BYTES - 1;
        check_doca(doca_dpa_mem_alloc(r.dpa, r.ptr_chasing_alloc_bytes, &r.ptr_chasing_alloc_dev_ptr),
                   "doca_dpa_mem_alloc(pointer_chasing)");
        r.ptr_chasing_base_dev_ptr =
            align_up(r.ptr_chasing_alloc_dev_ptr, DPA_OFFLOADING_PTR_CHASING_STRIDE_BYTES);

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

    check_doca(doca_dpa_mem_alloc(r.dpa, sizeof(struct dpa_offloading_persistent_thread_arg), &r.thread_arg_dev),
               "doca_dpa_mem_alloc(thread_arg)");
    struct dpa_offloading_persistent_thread_arg arg = {};
    arg.dpa_ctx_handle = r.dpa_handle;
    arg.shm_mmap_handle = r.shm_dpa_handle;
    arg.shm_addr = reinterpret_cast<uint64_t>(r.shm_base);
    arg.ptr_chasing_base = static_cast<uint64_t>(r.ptr_chasing_base_dev_ptr);
    arg.self_notify_handle = 0;
    check_doca(doca_dpa_h2d_memcpy(r.dpa, r.thread_arg_dev, &arg, sizeof(arg)),
               "doca_dpa_h2d_memcpy(thread_arg)");

    if (cfg.affinity_on) {
        check_doca(doca_dpa_eu_affinity_create(r.dpa, &r.affinity), "doca_dpa_eu_affinity_create");
        check_doca(doca_dpa_eu_affinity_set(r.affinity, cfg.eu_id), "doca_dpa_eu_affinity_set");
    }

    check_doca(doca_dpa_thread_create(r.dpa, &r.thread), "doca_dpa_thread_create");
    check_doca(doca_dpa_thread_set_func_arg(r.thread, &dpa_offloading_persistent_thread,
                                             static_cast<uint64_t>(r.thread_arg_dev)),
               "doca_dpa_thread_set_func_arg");
    if (r.affinity != nullptr) {
        check_doca(doca_dpa_thread_set_affinity(r.thread, r.affinity), "doca_dpa_thread_set_affinity");
    }
    check_doca(doca_dpa_thread_start(r.thread), "doca_dpa_thread_start");

    check_doca(doca_dpa_notification_completion_create(r.dpa, r.thread, &r.notify_comp),
               "doca_dpa_notification_completion_create");
    check_doca(doca_dpa_notification_completion_start(r.notify_comp),
               "doca_dpa_notification_completion_start");
    check_doca(doca_dpa_notification_completion_get_dpa_handle(r.notify_comp, &r.notify_handle),
               "doca_dpa_notification_completion_get_dpa_handle");

    // set self_notify, make the thread runnable and kick it
    uint64_t self_notify = r.notify_handle;
    check_doca(doca_dpa_h2d_memcpy(r.dpa,
                                   r.thread_arg_dev + offsetof(dpa_offloading_persistent_thread_arg, self_notify_handle),
                                   &self_notify, sizeof(self_notify)),
               "doca_dpa_h2d_memcpy(self_notify)");

    check_doca(doca_dpa_thread_run(r.thread), "doca_dpa_thread_run");
    uint64_t ret = 0;
    check_doca(doca_dpa_rpc(r.dpa, &dpa_offloading_persistent_kickstart_rpc, &ret, r.dpa_handle, r.notify_handle),
               "doca_dpa_rpc(kickstart)");
}

// one request and response per iteration

void launch_offload(PersistentResources &r, uint32_t kernel_kind, uint32_t work_iterations)
{
    ++r.request_seq;
    r.shm->command.kernel_kind = kernel_kind;
    r.shm->command.work_iterations = work_iterations;
    std::atomic_thread_fence(std::memory_order_release);
    const char *flush_base = reinterpret_cast<const char *>(&r.shm->command);
    for (size_t off = 0; off < sizeof(r.shm->command); off += 64) {
        _mm_clflush(flush_base + off);
    }
    _mm_mfence();
    std::atomic_thread_fence(std::memory_order_release);
    r.shm->doorbell.request_seq = r.request_seq;
    _mm_clflush(&r.shm->doorbell);
    _mm_mfence();
}

void wait_for_offload(PersistentResources &r)
{
    // poll done_seq in shared memory
    // the worker writes it back through the host window
    volatile const uint64_t *done = &r.shm->response.done_seq;
    const uint64_t target = r.request_seq;
    while (*done < target) {
    }
}

uint64_t read_kernel_cycles(PersistentResources &r)
{
    return r.shm->response.kernel_cycles;
}

OffloadStats measure_offloaded_workload(const PersistentConfig &cfg,
                                        PersistentResources &r,
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
        launch_offload(r, cfg.kernel_kind, work_iterations);
        if (sampled_overlap_delay_us > 0.0) {
            const auto delay = std::chrono::duration_cast<HostClock::duration>(
                std::chrono::duration<double, std::micro>(sampled_overlap_delay_us));
            const auto deadline = HostClock::now() + delay;
            while (HostClock::now() < deadline) {
            }
        }
        wait_for_offload(r);
        const auto end = HostClock::now();

        const uint64_t kernel_cycles = read_kernel_cycles(r);
        if (cfg.verify && cfg.kernel_kind == DPA_OFFLOADING_KERNEL_COMPUTE &&
            r.shm->response.result != expected) {
            throw_runtime("DPA offload verify failed");
        }

        if (iter >= cfg.warmup) {
            wall_samples.push_back(elapsed_us(start, end));
            dpa_samples.push_back(static_cast<double>(kernel_cycles) / static_cast<double>(cfg.clock_mhz));
        }
    }

    return {compute_stats(std::move(wall_samples)), compute_stats(std::move(dpa_samples))};
}

void print_metadata(const PersistentConfig &cfg)
{
    std::cout << "device=" << cfg.device_name
              << " mode=persistent"
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
              << " affinity=" << (cfg.affinity_on ? 1 : 0)
              << " eu_id=" << (cfg.affinity_on ? cfg.eu_id : 0)
              << " overlap_delay_distribution=" << (cfg.random_overlap_delay ? "normal" : "fixed")
              << " overlap_delay_stddev_fraction_values=" << join_csv(cfg.overlap_delay_stddev_fractions)
              << " warmup=" << cfg.warmup
              << " iters=" << cfg.iters
              << " clock_mhz=" << cfg.clock_mhz
              << " verify=" << (cfg.verify ? 1 : 0)
              << "\n";
}

void print_persistent_header()
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

void print_persistent_row(uint32_t rate,
                          const PersistentConfig &cfg,
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

void run_benchmark(const PersistentConfig &cfg)
{
    PersistentResources res{};
    try {
        init_persistent(cfg, res);
        std::vector<dpa_offloading_ptr_chasing_node> host_ptr_chasing_nodes;
        if (cfg.kernel_kind == DPA_OFFLOADING_KERNEL_POINTER_CHASING) {
            host_ptr_chasing_nodes = make_host_pointer_chasing_loop();
        } else {
            prepare_inputs(cfg, res);
        }

        print_metadata(cfg);
        if (cfg.print_header) {
            print_persistent_header();
        }

        for (uint32_t work_iterations : workload_values(cfg)) {
            const uint32_t expected =
                cfg.kernel_kind == DPA_OFFLOADING_KERNEL_COMPUTE ? expected_result(cfg.seed, work_iterations) : 0U;
            const Stats host_stats =
                measure_host_workload(cfg, res, host_ptr_chasing_nodes, work_iterations, expected);
            if (host_stats.avg <= 0.0) {
                throw_runtime("Measured host workload time is zero");
            }

            for (double overlap_delay_stddev_fraction : cfg.overlap_delay_stddev_fractions) {
                for (uint32_t rate : cfg.offload_rates) {
                    const double rate_fraction = static_cast<double>(rate) / 100.0;
                    const double modeled_total_us = host_stats.avg / rate_fraction;
                    const double overlap_delay_us = modeled_total_us - host_stats.avg;
                    const OffloadStats offload_stats = measure_offloaded_workload(cfg,
                                                                                  res,
                                                                                  work_iterations,
                                                                                  overlap_delay_us,
                                                                                  overlap_delay_stddev_fraction,
                                                                                  expected);
                    print_persistent_row(rate,
                                         cfg,
                                         work_iterations,
                                         host_stats,
                                         modeled_total_us,
                                         overlap_delay_us,
                                         overlap_delay_stddev_fraction,
                                         offload_stats,
                                         expected);
                }
            }
        }

        cleanup_persistent(res);
    } catch (...) {
        cleanup_persistent(res);
        throw;
    }
}

} // namespace

int main(int argc, char **argv)
{
    try {
        init_doca_log();
        const PersistentConfig cfg = parse_persistent_args(argc, argv);
        run_benchmark(cfg);
        return EXIT_SUCCESS;
    } catch (const std::exception &e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return EXIT_FAILURE;
    }
}
