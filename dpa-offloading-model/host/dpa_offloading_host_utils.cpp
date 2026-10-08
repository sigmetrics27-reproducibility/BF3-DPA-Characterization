#include "dpa_offloading_host_utils.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>

#include <doca_log.h>

extern struct doca_dpa_app *DPA_APP_NAME;

namespace {

constexpr size_t kHostAlignment = 4096;
constexpr size_t kExternalPtrLineBytes = 64;
constexpr uint64_t kSyncEventMask = UINT64_MAX;
constexpr uint64_t kCompletionTimeoutUs = 10ULL * 60ULL * 1000ULL * 1000ULL;

uint64_t align_up(uint64_t value, uint64_t align)
{
    const uint64_t rem = value % align;
    return (rem == 0) ? value : value + (align - rem);
}

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

std::vector<uint32_t> parse_rate_list(const std::string &csv)
{
    std::vector<uint32_t> rates;
    std::stringstream ss(csv);
    std::string token;

    while (std::getline(ss, token, ',')) {
        if (token.empty()) {
            continue;
        }
        const uint64_t rate = parse_u64(token, "--offload-rates");
        if (rate == 0 || rate > 100) {
            throw_runtime("--offload-rates entries must be in [1, 100]");
        }
        rates.push_back(static_cast<uint32_t>(rate));
    }

    if (rates.empty()) {
        throw_runtime("--offload-rates must not be empty");
    }

    std::sort(rates.begin(), rates.end());
    rates.erase(std::unique(rates.begin(), rates.end()), rates.end());
    return rates;
}

std::vector<uint32_t> parse_adds_per_element_list(const std::string &csv)
{
    std::vector<uint32_t> values;
    std::stringstream ss(csv);
    std::string token;

    while (std::getline(ss, token, ',')) {
        if (token.empty()) {
            continue;
        }
        const uint64_t value = parse_u64(token, "--adds-per-element");
        if (value == 0 || value > std::numeric_limits<uint32_t>::max()) {
            throw_runtime("--adds-per-element entries must be positive uint32 values");
        }
        values.push_back(static_cast<uint32_t>(value));
    }

    if (values.empty()) {
        throw_runtime("--adds-per-element must not be empty");
    }

    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
}

bool parse_on_off(const std::string &value, const char *name)
{
    if (value == "on") {
        return true;
    }
    if (value == "off") {
        return false;
    }
    throw_runtime(std::string(name) + " must be either 'on' or 'off'");
}

std::vector<uint32_t> parse_ptr_chasing_iters_list(const std::string &csv)
{
    std::vector<uint32_t> values;
    std::stringstream ss(csv);
    std::string token;

    while (std::getline(ss, token, ',')) {
        if (token.empty()) {
            continue;
        }
        const uint64_t value = parse_u64(token, "--ptr-chasing-iters");
        if (value == 0 || value > std::numeric_limits<uint32_t>::max()) {
            throw_runtime("--ptr-chasing-iters entries must be positive uint32 values");
        }
        values.push_back(static_cast<uint32_t>(value));
    }

    if (values.empty()) {
        throw_runtime("--ptr-chasing-iters must not be empty");
    }

    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
}

std::vector<double> parse_overlap_delay_stddev_fraction_list(const std::string &csv)
{
    std::vector<double> values;
    std::stringstream ss(csv);
    std::string token;

    while (std::getline(ss, token, ',')) {
        if (token.empty()) {
            continue;
        }
        const double value = parse_double(token, "--overlap-delay-stddev-fraction");
        if (value < 0.0) {
            throw_runtime("--overlap-delay-stddev-fraction entries must be >= 0");
        }
        values.push_back(value);
    }

    if (values.empty()) {
        throw_runtime("--overlap-delay-stddev-fraction must not be empty");
    }

    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
}

size_t percentile_index(size_t n, double p)
{
    if (n == 0) {
        return 0;
    }
    const double rank = std::ceil(p * static_cast<double>(n));
    size_t idx = (rank <= 1.0) ? 0 : static_cast<size_t>(rank - 1.0);
    if (idx >= n) {
        idx = n - 1;
    }
    return idx;
}

void destroy_sync_event(SyncEventResource &se)
{
    if (se.event != nullptr) {
        (void)doca_sync_event_stop(se.event);
        (void)doca_sync_event_destroy(se.event);
        se.event = nullptr;
        se.dpa_handle = 0;
    }
}

void create_dpa_to_cpu_sync_event(const Resources &r, bool need_dpa_handle, SyncEventResource &se)
{
    check_doca(doca_sync_event_create(&se.event), "doca_sync_event_create");
    try {
        check_doca(doca_sync_event_add_publisher_location_dpa(se.event, r.dpa),
                   "doca_sync_event_add_publisher_location_dpa");
        check_doca(doca_sync_event_add_subscriber_location_cpu(se.event, r.dev),
                   "doca_sync_event_add_subscriber_location_cpu");
        check_doca(doca_sync_event_start(se.event), "doca_sync_event_start");
        if (need_dpa_handle) {
            check_doca(doca_sync_event_get_dpa_handle(se.event, r.dpa, &se.dpa_handle),
                       "doca_sync_event_get_dpa_handle");
        }
    } catch (...) {
        destroy_sync_event(se);
        throw;
    }
}

void wait_sync_event_gt_with_timeout(const char *what,
                                     doca_sync_event *event,
                                     uint64_t threshold,
                                     uint64_t timeout_us)
{
    const auto t0 = HostClock::now();
    uint64_t value = 0;

    while (true) {
        check_doca(doca_sync_event_get(event, &value), "doca_sync_event_get(timeout_poll)");
        if ((value & kSyncEventMask) > (threshold & kSyncEventMask)) {
            return;
        }

        const auto elapsed =
            std::chrono::duration_cast<std::chrono::microseconds>(HostClock::now() - t0).count();
        if (elapsed > static_cast<long long>(timeout_us)) {
            throw_runtime(std::string(what) + " timed out");
        }
    }
}

} // namespace

[[noreturn]] void throw_runtime(const std::string &msg)
{
    throw std::runtime_error(msg);
}

void check_doca(doca_error_t status, const char *what)
{
    if (status != DOCA_SUCCESS) {
        throw_runtime(std::string(what) + " failed: " + doca_error_get_descr(status));
    }
}

double elapsed_us(HostClock::time_point start, HostClock::time_point end)
{
    return std::chrono::duration<double, std::micro>(end - start).count();
}

void open_device_by_ibdev_name(const std::string &device_name, doca_dev **dev)
{
    struct doca_devinfo **devinfo_list = nullptr;
    uint32_t nb_devs = 0;
    check_doca(doca_devinfo_create_list(&devinfo_list, &nb_devs), "doca_devinfo_create_list");

    char ibdev_name[DOCA_DEVINFO_IBDEV_NAME_SIZE + 1] = {0};
    bool found = false;

    for (uint32_t i = 0; i < nb_devs; ++i) {
        const doca_error_t st = doca_devinfo_get_ibdev_name(devinfo_list[i], ibdev_name, DOCA_DEVINFO_IBDEV_NAME_SIZE);
        if (st != DOCA_SUCCESS) {
            continue;
        }
        if (device_name == ibdev_name) {
            check_doca(doca_dev_open(devinfo_list[i], dev), "doca_dev_open");
            found = true;
            break;
        }
    }

    check_doca(doca_devinfo_destroy_list(devinfo_list), "doca_devinfo_destroy_list");

    if (!found || *dev == nullptr) {
        throw_runtime("IB device not found: " + device_name);
    }
}

void init_doca_log()
{
    check_doca(doca_log_backend_create_standard(), "doca_log_backend_create_standard");

    struct doca_log_backend *sdk_log = nullptr;
    check_doca(doca_log_backend_create_with_file_sdk(stderr, &sdk_log), "doca_log_backend_create_with_file_sdk");
    check_doca(doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING), "doca_log_backend_set_sdk_level");
}

void print_usage(const char *prog)
{
    std::cerr
        << "Usage:\n"
        << "  " << prog << " --device mlx5_0 [options]\n\n"
        << "Options:\n"
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
        << "  --verify                     Verify host and DPA scalar results\n"
        << "  --no-header                  Suppress CSV header line\n";
}

Config parse_args(int argc, char **argv)
{
    Config cfg;

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
            cfg.offload_rates = parse_rate_list(next("--offload-rates"));
        } else if (arg == "--adds-per-element") {
            cfg.adds_per_element_values = parse_adds_per_element_list(next("--adds-per-element"));
        } else if (arg == "--ptr-chasing-iters") {
            cfg.ptr_chasing_iters_values = parse_ptr_chasing_iters_list(next("--ptr-chasing-iters"));
        } else if (arg == "--warmup") {
            cfg.warmup = static_cast<uint32_t>(parse_u64(next("--warmup"), "--warmup"));
        } else if (arg == "--iters") {
            cfg.iters = static_cast<uint32_t>(parse_u64(next("--iters"), "--iters"));
        } else if (arg == "--clock-mhz") {
            cfg.clock_mhz = static_cast<uint32_t>(parse_u64(next("--clock-mhz"), "--clock-mhz"));
        } else if (arg == "--seed") {
            cfg.seed = static_cast<uint32_t>(parse_u64(next("--seed"), "--seed"));
        } else if (arg == "--random-overlap-delay") {
            cfg.random_overlap_delay = parse_on_off(next("--random-overlap-delay"), "--random-overlap-delay");
        } else if (arg == "--overlap-delay-stddev-fraction") {
            cfg.overlap_delay_stddev_fractions =
                parse_overlap_delay_stddev_fraction_list(next("--overlap-delay-stddev-fraction"));
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

    if (cfg.iters == 0) {
        throw_runtime("--iters must be > 0");
    }
    if (cfg.clock_mhz == 0) {
        throw_runtime("--clock-mhz must be > 0");
    }

    return cfg;
}

Stats compute_stats(std::vector<double> samples)
{
    if (samples.empty()) {
        throw_runtime("Cannot compute statistics for an empty sample set");
    }

    double sum = 0.0;
    for (double sample : samples) {
        sum += sample;
    }

    Stats stats{};
    stats.avg = sum / static_cast<double>(samples.size());

    if (samples.size() > 1) {
        double sum_sq = 0.0;
        for (double sample : samples) {
            const double delta = sample - stats.avg;
            sum_sq += delta * delta;
        }
        stats.stddev = std::sqrt(sum_sq / static_cast<double>(samples.size() - 1));
    }

    std::sort(samples.begin(), samples.end());
    stats.min = samples.front();
    stats.p50 = samples[percentile_index(samples.size(), 0.50)];
    stats.p95 = samples[percentile_index(samples.size(), 0.95)];
    stats.p99 = samples[percentile_index(samples.size(), 0.99)];
    stats.max = samples.back();
    return stats;
}

std::string join_csv(const std::vector<uint32_t> &values)
{
    std::ostringstream out;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            out << ",";
        }
        out << values[i];
    }
    return out.str();
}

std::string join_csv(const std::vector<double> &values)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(6);
    for (size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            out << ",";
        }
        out << values[i];
    }
    return out.str();
}

uint32_t input_value(uint32_t seed, uint32_t i)
{
    return (seed * 2654435761U + i * 17U + 0x12345U) | 1U;
}

void prepare_inputs(const Config &cfg, Resources &r)
{
    for (uint32_t i = 0; i < DPA_OFFLOADING_ELEMENTS; ++i) {
        r.host_region[i] = input_value(cfg.seed, i);
    }
    *r.host_result = 0;
}

void busy_wait_for_us(double delay_us)
{
    if (delay_us <= 0.0) {
        return;
    }

    const auto delay =
        std::chrono::duration_cast<HostClock::duration>(std::chrono::duration<double, std::micro>(delay_us));
    const auto deadline = HostClock::now() + delay;
    while (HostClock::now() < deadline) {
    }
}

uint64_t us_to_cycles(double delay_us, uint32_t clock_mhz)
{
    if (delay_us <= 0.0) {
        return 0;
    }
    const double cycles = delay_us * static_cast<double>(clock_mhz);
    if (cycles > static_cast<double>(std::numeric_limits<uint64_t>::max())) {
        throw_runtime("Delay is too large to convert to DPA cycles");
    }
    return static_cast<uint64_t>(cycles);
}

void cleanup_resources(Resources &r)
{
    destroy_sync_event(r.kernel_cycles_event);
    destroy_sync_event(r.completion_event);

    if (r.ptr_chasing_alloc_dev_ptr != 0 && r.dpa != nullptr) {
        (void)doca_dpa_mem_free(r.dpa, r.ptr_chasing_alloc_dev_ptr);
        r.ptr_chasing_alloc_dev_ptr = 0;
        r.ptr_chasing_base_dev_ptr = 0;
        r.ptr_chasing_alloc_bytes = 0;
    }

    if (r.host_mmap != nullptr) {
        (void)doca_mmap_destroy(r.host_mmap);
        r.host_mmap = nullptr;
        r.host_dpa_mmap_handle = 0;
    }
    if (r.host_mem_base != nullptr) {
        free(r.host_mem_base);
        r.host_mem_base = nullptr;
        r.host_region = nullptr;
        r.host_result = nullptr;
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

void init_resources(const Config &cfg, Resources &r)
{
    open_device_by_ibdev_name(cfg.device_name, &r.dev);

    check_doca(doca_dpa_create(r.dev, &r.dpa), "doca_dpa_create");
    check_doca(doca_dpa_set_app(r.dpa, DPA_APP_NAME), "doca_dpa_set_app");
    check_doca(doca_dpa_start(r.dpa), "doca_dpa_start");
    check_doca(doca_dpa_get_dpa_handle(r.dpa, &r.dpa_handle), "doca_dpa_get_dpa_handle");

    create_dpa_to_cpu_sync_event(r, false, r.completion_event);
    create_dpa_to_cpu_sync_event(r, true, r.kernel_cycles_event);

    const size_t useful_bytes = (DPA_OFFLOADING_ELEMENTS + 1) * sizeof(uint32_t);
    r.host_mem_bytes = static_cast<size_t>(align_up(useful_bytes, kExternalPtrLineBytes));

    void *host_mem = nullptr;
    if (posix_memalign(&host_mem, kHostAlignment, r.host_mem_bytes) != 0 || host_mem == nullptr) {
        throw_runtime("posix_memalign failed for host memory");
    }
    r.host_mem_base = host_mem;
    r.host_region = static_cast<uint32_t *>(host_mem);
    r.host_result = r.host_region + DPA_OFFLOADING_ELEMENTS;
    std::memset(r.host_mem_base, 0, r.host_mem_bytes);

    check_doca(doca_mmap_create(&r.host_mmap), "doca_mmap_create(host)");
    try {
        check_doca(doca_mmap_set_permissions(r.host_mmap, DOCA_ACCESS_FLAG_LOCAL_READ_WRITE),
                   "doca_mmap_set_permissions(host)");
        check_doca(doca_mmap_set_memrange(r.host_mmap, r.host_mem_base, r.host_mem_bytes),
                   "doca_mmap_set_memrange(host)");
        check_doca(doca_mmap_add_dev(r.host_mmap, r.dev), "doca_mmap_add_dev(host)");
        check_doca(doca_mmap_start(r.host_mmap), "doca_mmap_start(host)");
        check_doca(doca_mmap_dev_get_dpa_handle(r.host_mmap, r.dev, &r.host_dpa_mmap_handle),
                   "doca_mmap_dev_get_dpa_handle(host)");
    } catch (...) {
        if (r.host_mmap != nullptr) {
            (void)doca_mmap_destroy(r.host_mmap);
            r.host_mmap = nullptr;
        }
        throw;
    }

    if (cfg.kernel_kind == DPA_OFFLOADING_KERNEL_POINTER_CHASING) {
        r.ptr_chasing_alloc_bytes =
            DPA_OFFLOADING_PTR_CHASING_BYTES + DPA_OFFLOADING_PTR_CHASING_STRIDE_BYTES - 1;
        check_doca(doca_dpa_mem_alloc(r.dpa, r.ptr_chasing_alloc_bytes, &r.ptr_chasing_alloc_dev_ptr),
                   "doca_dpa_mem_alloc(pointer_chasing)");
        r.ptr_chasing_base_dev_ptr =
            align_up(r.ptr_chasing_alloc_dev_ptr, DPA_OFFLOADING_PTR_CHASING_STRIDE_BYTES);
    }
}

void wait_for_offload_completion(Resources &r)
{
    wait_sync_event_gt_with_timeout("DPA offload completion",
                                    r.completion_event.event,
                                    r.seq - 1,
                                    kCompletionTimeoutUs);
}

uint64_t read_kernel_cycles(Resources &r)
{
    uint64_t kernel_cycles = 0;
    check_doca(doca_sync_event_get(r.kernel_cycles_event.event, &kernel_cycles),
               "doca_sync_event_get(kernel_cycles)");
    return kernel_cycles;
}

void print_header()
{
    std::cout
        << "offload_rate_percent,adds_per_element,workload_delay_us,elements,"
        << "host_workload_avg_us,host_workload_stddev_us,"
        << "host_workload_min_us,host_workload_p50_us,host_workload_p95_us,host_workload_p99_us,"
        << "host_workload_max_us,theoretical_host_total_us,overlap_delay_us,"
        << "offloaded_wall_avg_us,offloaded_wall_stddev_us,offloaded_wall_min_us,"
        << "offloaded_wall_p50_us,offloaded_wall_p95_us,offloaded_wall_p99_us,offloaded_wall_max_us,"
        << "dpa_kernel_avg_us,dpa_kernel_stddev_us,dpa_kernel_min_us,dpa_kernel_p50_us,"
        << "dpa_kernel_p95_us,dpa_kernel_p99_us,dpa_kernel_max_us,"
        << "offloading_efficiency_avg,offloading_efficiency_stddev,result,status\n";
}

void print_row(uint32_t rate,
               uint32_t adds_per_element,
               double workload_delay_us,
               const Stats &host,
               double theoretical_host_total_us,
               double overlap_delay_us,
               const OffloadStats &offload,
               uint32_t result)
{
    const double efficiency_avg = offload.wall_us.avg / theoretical_host_total_us;
    const double efficiency_stddev = offload.wall_us.stddev / theoretical_host_total_us;

    std::cout << rate
              << "," << adds_per_element
              << "," << std::fixed << std::setprecision(6) << workload_delay_us
              << "," << DPA_OFFLOADING_ELEMENTS
              << "," << host.avg
              << "," << host.stddev
              << "," << host.min
              << "," << host.p50
              << "," << host.p95
              << "," << host.p99
              << "," << host.max
              << "," << theoretical_host_total_us
              << "," << overlap_delay_us
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
