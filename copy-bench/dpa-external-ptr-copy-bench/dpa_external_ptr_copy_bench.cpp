#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <doca_dev.h>
#include <doca_dpa.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_mmap.h>

#include "host/dpa_external_ptr_copy_common.h"

extern "C" {
    doca_dpa_func_t dpa_external_ptr_copy_run_rpc;
}

extern struct doca_dpa_app *DPA_APP_NAME;

DOCA_LOG_REGISTER(DPA_EXTERNAL_PTR_COPY_BENCH);

namespace {

constexpr uint64_t kDefaultMinPayload = 1;
constexpr uint64_t kDefaultMaxPayload = 1ULL << 20;  // 1 MiB
constexpr double kDefaultSizeFactor = 2.0;
constexpr uint32_t kDefaultWarmup = 20;
constexpr uint32_t kDefaultIters = 200;
constexpr uint32_t kDefaultClockMHz = 1800;
constexpr uint32_t kDefaultSeed = 1;
constexpr uint32_t kVerifyChunkBytes = 64 * 1024;
constexpr size_t kHostAlignment = 4096;
constexpr size_t kExternalPtrLineBytes = 64;

struct Config {
    std::string device_name = "mlx5_0";
    uint32_t mode = DPA_EXTERNAL_PTR_COPY_MODE_H2D;

    uint64_t min_payload = kDefaultMinPayload;
    uint64_t max_payload = kDefaultMaxPayload;
    double size_factor = kDefaultSizeFactor;

    uint32_t warmup = kDefaultWarmup;
    uint32_t iters = kDefaultIters;
    uint32_t clock_mhz = kDefaultClockMHz;
    uint32_t seed = kDefaultSeed;
    bool verify = false;
};

struct Resources {
    doca_dev *dev = nullptr;
    doca_dpa *dpa = nullptr;
    doca_dpa_dev_t dpa_handle = 0;

    void *host_mem_base = nullptr;
    size_t host_mem_bytes = 0;
    size_t host_region_bytes = 0;

    uint8_t *host_src = nullptr;
    uint8_t *host_dst = nullptr;

    doca_mmap *host_mmap = nullptr;
    doca_dpa_dev_mmap_t host_dpa_mmap_handle = 0;

    doca_dpa_dev_uintptr_t local_dpa_buf = 0;
    size_t local_dpa_buf_bytes = 0;

    uint8_t *verify_buf = nullptr;
    size_t verify_buf_bytes = 0;
};

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

uint64_t align_up(uint64_t value, uint64_t align)
{
    if (align == 0) {
        return value;
    }
    uint64_t rem = value % align;
    if (rem == 0) {
        return value;
    }
    return value + (align - rem);
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
    double v = 0;
    try {
        v = std::stod(s, &idx);
    } catch (const std::exception &) {
        throw_runtime(std::string("Invalid ") + name + ": " + s);
    }
    if (idx != s.size()) {
        throw_runtime(std::string("Invalid ") + name + ": " + s);
    }
    return v;
}

uint32_t parse_mode(const std::string &s)
{
    if (s == "h2d") {
        return DPA_EXTERNAL_PTR_COPY_MODE_H2D;
    }
    if (s == "d2h") {
        return DPA_EXTERNAL_PTR_COPY_MODE_D2H;
    }
    throw_runtime("Unsupported mode: " + s + " (expected: h2d|d2h)");
}

const char *mode_to_string(uint32_t mode)
{
    switch (mode) {
    case DPA_EXTERNAL_PTR_COPY_MODE_H2D:
        return "h2d";
    case DPA_EXTERNAL_PTR_COPY_MODE_D2H:
        return "d2h";
    default:
        return "unknown";
    }
}

void print_usage(const char *prog)
{
    std::cerr
        << "Usage:\n"
        << "  " << prog << " --device mlx5_0 [options]\n\n"
        << "Options:\n"
        << "  --device <name>                 Device name (default: mlx5_0)\n"
        << "  --mode h2d|d2h                  Copy direction (default: h2d)\n"
        << "  --min-payload <bytes>           Min payload bytes (default: 1)\n"
        << "  --max-payload <bytes>           Max payload bytes (default: 1048576)\n"
        << "  --size-factor <float>           Payload growth factor (default: 2.0)\n"
        << "  --warmup <num>                  Warmup iterations per payload (default: 20)\n"
        << "  --iters <num>                   Measured iterations per payload (default: 200)\n"
        << "  --clock-mhz <num>               DPA clock MHz for ns conversion (default: 1800)\n"
        << "  --seed <num>                    Seed for deterministic fill pattern (default: 1)\n"
        << "  --verify                        Verify payload content after each run\n";
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

        if (arg == "--device") {
            cfg.device_name = next("--device");
        } else if (arg == "--mode") {
            cfg.mode = parse_mode(next("--mode"));
        } else if (arg == "--min-payload") {
            cfg.min_payload = parse_u64(next("--min-payload"), "--min-payload");
        } else if (arg == "--max-payload") {
            cfg.max_payload = parse_u64(next("--max-payload"), "--max-payload");
        } else if (arg == "--size-factor") {
            cfg.size_factor = parse_double(next("--size-factor"), "--size-factor");
        } else if (arg == "--warmup") {
            cfg.warmup = static_cast<uint32_t>(parse_u64(next("--warmup"), "--warmup"));
        } else if (arg == "--iters") {
            cfg.iters = static_cast<uint32_t>(parse_u64(next("--iters"), "--iters"));
        } else if (arg == "--clock-mhz") {
            cfg.clock_mhz = static_cast<uint32_t>(parse_u64(next("--clock-mhz"), "--clock-mhz"));
        } else if (arg == "--seed") {
            cfg.seed = static_cast<uint32_t>(parse_u64(next("--seed"), "--seed"));
        } else if (arg == "--verify") {
            cfg.verify = true;
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            std::exit(EXIT_SUCCESS);
        } else {
            throw_runtime("Unknown argument: " + arg);
        }
    }

    if (cfg.min_payload == 0) {
        throw_runtime("--min-payload must be > 0");
    }
    if (cfg.max_payload < cfg.min_payload) {
        throw_runtime("--max-payload must be >= --min-payload");
    }
    if (cfg.size_factor <= 1.0) {
        throw_runtime("--size-factor must be > 1.0");
    }
    if (cfg.clock_mhz == 0) {
        throw_runtime("--clock-mhz must be > 0");
    }
    if (cfg.max_payload > std::numeric_limits<uint32_t>::max()) {
        throw_runtime("--max-payload must fit in uint32");
    }

    const uint64_t total_iters = static_cast<uint64_t>(cfg.warmup) + static_cast<uint64_t>(cfg.iters);
    if (total_iters > std::numeric_limits<uint32_t>::max()) {
        throw_runtime("warmup + iters exceeds uint32 range");
    }

    return cfg;
}

std::vector<uint64_t> build_payload_list(const Config &cfg)
{
    std::vector<uint64_t> payloads;
    uint64_t cur = cfg.min_payload;

    while (cur <= cfg.max_payload) {
        payloads.push_back(cur);

        uint64_t next = static_cast<uint64_t>(static_cast<double>(cur) * cfg.size_factor);
        if (next <= cur) {
            next = cur + 1;
        }
        if (next > cfg.max_payload && cur != cfg.max_payload) {
            payloads.push_back(cfg.max_payload);
            break;
        }
        cur = next;
    }

    if (payloads.empty()) {
        payloads.push_back(cfg.max_payload);
    }

    payloads.erase(std::unique(payloads.begin(), payloads.end()), payloads.end());
    return payloads;
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

void cleanup_resources(Resources &r)
{
    if (r.local_dpa_buf != 0 && r.dpa != nullptr) {
        (void)doca_dpa_mem_free(r.dpa, r.local_dpa_buf);
        r.local_dpa_buf = 0;
    }

    if (r.host_mmap != nullptr) {
        (void)doca_mmap_destroy(r.host_mmap);
        r.host_mmap = nullptr;
    }

    if (r.verify_buf != nullptr) {
        free(r.verify_buf);
        r.verify_buf = nullptr;
    }

    if (r.host_mem_base != nullptr) {
        free(r.host_mem_base);
        r.host_mem_base = nullptr;
        r.host_src = nullptr;
        r.host_dst = nullptr;
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

    r.host_region_bytes = static_cast<size_t>(align_up(std::max<uint64_t>(cfg.max_payload, kExternalPtrLineBytes),
                                                        kExternalPtrLineBytes));
    r.host_mem_bytes = r.host_region_bytes * 2;

    void *host_mem = nullptr;
    if (posix_memalign(&host_mem, kHostAlignment, r.host_mem_bytes) != 0 || host_mem == nullptr) {
        throw_runtime("posix_memalign failed for host memory");
    }
    r.host_mem_base = host_mem;
    std::memset(r.host_mem_base, 0, r.host_mem_bytes);

    r.host_src = static_cast<uint8_t *>(r.host_mem_base);
    r.host_dst = r.host_src + r.host_region_bytes;

    check_doca(doca_mmap_create(&r.host_mmap), "doca_mmap_create(host)");
    check_doca(doca_mmap_set_permissions(r.host_mmap, DOCA_ACCESS_FLAG_LOCAL_READ_WRITE),
               "doca_mmap_set_permissions(host)");
    check_doca(doca_mmap_set_memrange(r.host_mmap, r.host_mem_base, r.host_mem_bytes),
               "doca_mmap_set_memrange(host)");
    check_doca(doca_mmap_add_dev(r.host_mmap, r.dev), "doca_mmap_add_dev(host)");
    check_doca(doca_mmap_start(r.host_mmap), "doca_mmap_start(host)");
    check_doca(doca_mmap_dev_get_dpa_handle(r.host_mmap, r.dev, &r.host_dpa_mmap_handle),
               "doca_mmap_dev_get_dpa_handle(host)");

    r.local_dpa_buf_bytes = static_cast<size_t>(align_up(std::max<uint64_t>(cfg.max_payload, 8), 8));
    check_doca(doca_dpa_mem_alloc(r.dpa, r.local_dpa_buf_bytes, &r.local_dpa_buf),
               "doca_dpa_mem_alloc(local)");
    check_doca(doca_dpa_memset(r.dpa, r.local_dpa_buf, 0, r.local_dpa_buf_bytes),
               "doca_dpa_memset(local)");

    r.verify_buf_bytes = r.host_region_bytes;
    void *verify_mem = nullptr;
    if (posix_memalign(&verify_mem, kExternalPtrLineBytes, r.verify_buf_bytes) != 0 || verify_mem == nullptr) {
        throw_runtime("posix_memalign failed for verify buffer");
    }
    r.verify_buf = static_cast<uint8_t *>(verify_mem);
}

uint8_t fill_value_for_payload(const Config &cfg, uint32_t payload)
{
    return static_cast<uint8_t>((payload ^ (cfg.mode * 31U) ^ (cfg.seed * 17U)) & 0xFFU);
}

void prepare_payload_buffers(const Config &cfg, const Resources &r, uint32_t payload)
{
    const uint8_t fill = fill_value_for_payload(cfg, payload);

    if (cfg.mode == DPA_EXTERNAL_PTR_COPY_MODE_H2D) {
        std::memset(r.host_src, fill, payload);
        check_doca(doca_dpa_memset(r.dpa, r.local_dpa_buf, 0, payload), "doca_dpa_memset(h2d dst)");
    } else {
        std::memset(r.host_dst, 0, payload);
        check_doca(doca_dpa_memset(r.dpa, r.local_dpa_buf, fill, payload), "doca_dpa_memset(d2h src)");
    }
}

void verify_payload_result(const Config &cfg, const Resources &r, uint32_t payload)
{
    if (!cfg.verify) {
        return;
    }

    const uint8_t expected_fill = fill_value_for_payload(cfg, payload);

    if (cfg.mode == DPA_EXTERNAL_PTR_COPY_MODE_H2D) {
        std::memset(r.verify_buf, 0, payload);

        uint32_t offset = 0;
        while (offset < payload) {
            const uint32_t chunk = std::min<uint32_t>(kVerifyChunkBytes, payload - offset);
            check_doca(doca_dpa_d2h_memcpy(r.dpa, r.verify_buf + offset, r.local_dpa_buf + offset, chunk),
                       "doca_dpa_d2h_memcpy(h2d verify)");
            offset += chunk;
        }

        for (uint32_t i = 0; i < payload; ++i) {
            if (r.verify_buf[i] != expected_fill) {
                throw_runtime("verify(h2d) failed at byte " + std::to_string(i));
            }
        }
        return;
    }

    for (uint32_t i = 0; i < payload; ++i) {
        if (r.host_dst[i] != expected_fill) {
            throw_runtime("verify(d2h) failed at byte " + std::to_string(i));
        }
    }
}

uint64_t run_copy_rpc(const Config &cfg, const Resources &r, uint32_t payload)
{
    uint64_t cycles = 0;

    check_doca(doca_dpa_rpc(r.dpa,
                            &dpa_external_ptr_copy_run_rpc,
                            &cycles,
                            r.dpa_handle,
                            r.host_dpa_mmap_handle,
                            reinterpret_cast<uint64_t>(r.host_src),
                            reinterpret_cast<uint64_t>(r.host_dst),
                            r.local_dpa_buf,
                            cfg.mode,
                            cfg.warmup,
                            cfg.iters,
                            payload),
               "doca_dpa_rpc(dpa_external_ptr_copy_run_rpc)");

    return cycles;
}

void init_doca_log()
{
    check_doca(doca_log_backend_create_standard(), "doca_log_backend_create_standard");

    struct doca_log_backend *sdk_log = nullptr;
    check_doca(doca_log_backend_create_with_file_sdk(stderr, &sdk_log), "doca_log_backend_create_with_file_sdk");
    check_doca(doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING), "doca_log_backend_set_sdk_level");
}

void run_benchmark(const Config &cfg)
{
    Resources res{};

    try {
        init_resources(cfg, res);

        const auto payload_list = build_payload_list(cfg);

        std::cout << "mode=" << mode_to_string(cfg.mode)
                  << " device=" << cfg.device_name
                  << " warmup=" << cfg.warmup
                  << " iters=" << cfg.iters
                  << " verify=" << (cfg.verify ? 1 : 0)
                  << "\n";
        std::cout << "payload_bytes,avg_latency_ns,bandwidth_GBps,total_cycles\n";

        for (uint64_t payload_u64 : payload_list) {
            const uint32_t payload = static_cast<uint32_t>(payload_u64);

            prepare_payload_buffers(cfg, res, payload);
            const uint64_t cycles = run_copy_rpc(cfg, res, payload);
            verify_payload_result(cfg, res, payload);

            const double avg_ns = (cfg.iters == 0)
                                      ? 0.0
                                      : (static_cast<double>(cycles) * 1000.0 /
                                         static_cast<double>(cfg.clock_mhz) /
                                         static_cast<double>(cfg.iters));
            const double bandwidth_gbps = (avg_ns > 0.0)
                                              ? (static_cast<double>(payload) / avg_ns)
                                              : 0.0;

            std::cout << payload << "," << std::fixed << std::setprecision(2) << avg_ns << ","
                      << std::fixed << std::setprecision(3) << bandwidth_gbps << "," << cycles << "\n";
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
